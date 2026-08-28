#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

using usb_vault::card::CardCredentialRejected;
using usb_vault::card::CardRecordState;
using usb_vault::card::initialize_card_record_v1;
using usb_vault::card::inspect_card_record;
using usb_vault::card::kCardRecordV1EnvelopeSize;
using usb_vault::card::kCardRecordV1HeaderSize;
using usb_vault::card::kCardRecordV1PrimaryEnvelopeOffset;
using usb_vault::card::kCardRecordV1RootSecretSize;
using usb_vault::card::kCardRecordV1SecondaryEnvelopeOffset;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::kLegacyP1CardBlobSize;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::card::rewrap_card_record_v1;
using usb_vault::card::unlock_card_record_v1;
using usb_vault::security::SecureBuffer;

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition) {
    fail(message);
  }
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] SecureBuffer
secure_copy(const std::span<const std::byte> source) {
  SecureBuffer result(source.size());
  std::ranges::copy(source, result.bytes().begin());
  return result;
}

void expect_same_secret(const SecureBuffer &left, const SecureBuffer &right,
                        const char *const message) {
  expect(left.size() == kCardRecordV1RootSecretSize,
         "left root secret has wrong size");
  expect(right.size() == kCardRecordV1RootSecretSize,
         "right root secret has wrong size");
  expect(std::ranges::equal(left.bytes(), right.bytes()), message);
}

template <typename Action>
void expect_invalid_argument(Action &&action, const char *const message) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  }
  fail(message);
}

template <typename Action>
void expect_credential_rejected(Action &&action, const char *const message) {
  try {
    action();
  } catch (const CardCredentialRejected &) {
    return;
  }
  fail(message);
}

void test_public_state_classification_fails_closed() {
  SecureBuffer provisioned = provision_uninitialized_card_record_v1();
  expect(provisioned.size() == kCardRecordV1Size,
         "provisioning record has wrong size");
  expect(inspect_card_record(provisioned.bytes()) ==
             CardRecordState::provisioned_uninitialized,
         "exact provisioning record was not accepted");

  provisioned.bytes().back() = std::byte{1};
  expect(inspect_card_record(provisioned.bytes()) == CardRecordState::malformed,
         "modified provisioning record was treated as uninitialized");

  SecureBuffer all_zero(kCardRecordV1Size);
  expect(inspect_card_record(all_zero.bytes()) == CardRecordState::malformed,
         "all-zero record was treated as uninitialized");

  SecureBuffer legacy(kLegacyP1CardBlobSize);
  expect(inspect_card_record(legacy.bytes()) == CardRecordState::legacy_p1_blob,
         "legacy P1 blob was not classified separately");

  SecureBuffer truncated(kCardRecordV1Size - 1);
  expect(inspect_card_record(truncated.bytes()) == CardRecordState::malformed,
         "truncated record was not rejected");
}

void test_initialization_and_primary_unlock() {
  SecureBuffer initialized =
      initialize_card_record_v1(secure_text("daily-password-v1"));
  expect(initialized.size() == kCardRecordV1Size,
         "initialized record has wrong size");
  expect(inspect_card_record(initialized.bytes()) ==
             CardRecordState::initialized_v1,
         "initialized record framing was rejected");

  const auto primary = initialized.bytes().subspan(
      kCardRecordV1PrimaryEnvelopeOffset, kCardRecordV1EnvelopeSize);
  const auto secondary = initialized.bytes().subspan(
      kCardRecordV1SecondaryEnvelopeOffset, kCardRecordV1EnvelopeSize);
  expect(!std::ranges::equal(primary, secondary),
         "primary and random secondary envelopes are identical");

  SecureBuffer root = unlock_card_record_v1(initialized.bytes(),
                                            secure_text("daily-password-v1"));
  expect(root.size() == kCardRecordV1RootSecretSize,
         "primary unlock returned wrong root size");

  expect_credential_rejected(
      [&] {
        static_cast<void>(unlock_card_record_v1(
            initialized.bytes(), secure_text("wrong-password-v1")));
      },
      "wrong password was accepted");

  SecureBuffer damaged = secure_copy(initialized.bytes());
  damaged.bytes()[kCardRecordV1PrimaryEnvelopeOffset +
                  kCardRecordV1EnvelopeSize - 1] ^= std::byte{1};
  expect(inspect_card_record(damaged.bytes()) ==
             CardRecordState::initialized_v1,
         "ciphertext damage changed public record classification");
  expect_credential_rejected(
      [&] {
        static_cast<void>(unlock_card_record_v1(
            damaged.bytes(), secure_text("daily-password-v1")));
      },
      "damaged authentication tag was accepted");
}

void test_dual_rewrap_accepts_both_credentials_without_shape_change() {
  SecureBuffer initialized =
      initialize_card_record_v1(secure_text("daily-password-v2"));
  SecureBuffer original_root = unlock_card_record_v1(
      initialized.bytes(), secure_text("daily-password-v2"));

  SecureBuffer dual = rewrap_card_record_v1(
      initialized.bytes(), secure_text("daily-password-v2"),
      secure_text("lure-password-v2"), secure_text("daily-password-v2"));
  expect(dual.size() == initialized.size(), "rewrap changed fixed record size");
  expect(inspect_card_record(dual.bytes()) == CardRecordState::initialized_v1,
         "rewrapped record framing was rejected");
  expect(std::ranges::equal(
             dual.bytes().first(kCardRecordV1HeaderSize).first(8),
             initialized.bytes().first(kCardRecordV1HeaderSize).first(8)),
         "rewrap changed public format magic");

  SecureBuffer primary_root =
      unlock_card_record_v1(dual.bytes(), secure_text("lure-password-v2"));
  SecureBuffer secondary_root =
      unlock_card_record_v1(dual.bytes(), secure_text("daily-password-v2"));
  expect_same_secret(original_root, primary_root,
                     "primary credential changed root secret");
  expect_same_secret(original_root, secondary_root,
                     "secondary credential changed root secret");

  expect_invalid_argument(
      [&] {
        static_cast<void>(rewrap_card_record_v1(
            dual.bytes(), secure_text("daily-password-v2"),
            secure_text("same-password-v2"), secure_text("same-password-v2")));
      },
      "identical replacement passwords were accepted");
}

void test_invalid_password_input_and_header_versions() {
  SecureBuffer empty;
  expect_invalid_argument(
      [&] { static_cast<void>(initialize_card_record_v1(std::move(empty))); },
      "empty initialization password was accepted");

  const std::array invalid_utf8{std::byte{0xC0}, std::byte{0xAF}};
  expect_invalid_argument(
      [&] {
        static_cast<void>(
            initialize_card_record_v1(secure_copy(std::span(invalid_utf8))));
      },
      "invalid UTF-8 initialization password was accepted");

  SecureBuffer initialized =
      initialize_card_record_v1(secure_text("daily-password-v3"));
  SecureBuffer future = secure_copy(initialized.bytes());
  future.bytes()[8] = std::byte{2};
  expect(inspect_card_record(future.bytes()) ==
             CardRecordState::unsupported_version,
         "future version was not classified separately");

  SecureBuffer malformed = secure_copy(initialized.bytes());
  malformed.bytes()[12] = std::byte{0};
  expect(inspect_card_record(malformed.bytes()) == CardRecordState::malformed,
         "invalid v1 size field was not classified as malformed");
  expect_invalid_argument(
      [&] {
        static_cast<void>(unlock_card_record_v1(
            malformed.bytes(), secure_text("daily-password-v3")));
      },
      "malformed record reached password processing");
}

} // namespace

int main() {
  test_public_state_classification_fails_closed();
  test_initialization_and_primary_unlock();
  test_dual_rewrap_accepts_both_credentials_without_shape_change();
  test_invalid_password_input_and_header_versions();
  std::cout << "card_record_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
