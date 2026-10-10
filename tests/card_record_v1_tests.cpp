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

using usb_vault::card::append_card_password_v1;
using usb_vault::card::CardCredentialRejected;
using usb_vault::card::kCardRecordV1TertiaryEnvelopeOffset;
using usb_vault::card::kCardRecordV1ThreeSlotSize;
using usb_vault::card::CardRecordState;
using usb_vault::card::initialize_card_record_and_root_v1;
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
  auto material =
      initialize_card_record_and_root_v1(
          secure_text("daily-password-v1"));
  SecureBuffer &initialized = material.record;
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
  expect_same_secret(material.root_secret, root,
                     "initialization returned the wrong root secret");

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

[[nodiscard]] std::span<const std::byte>
envelope_at(const SecureBuffer &record, const std::size_t position) {
  return record.bytes().subspan(
      kCardRecordV1HeaderSize + (position * kCardRecordV1EnvelopeSize),
      kCardRecordV1EnvelopeSize);
}

void expect_opens(const SecureBuffer &record, const std::string_view password,
                  const SecureBuffer &root, const char *const message) {
  SecureBuffer opened = unlock_card_record_v1(record.bytes(),
                                              secure_text(password));
  expect_same_secret(root, opened, message);
}

void expect_rejected(const SecureBuffer &record,
                     const std::string_view password,
                     const char *const message) {
  expect_credential_rejected(
      [&] {
        static_cast<void>(
            unlock_card_record_v1(record.bytes(), secure_text(password)));
      },
      message);
}

// One append, checked against the rule: [new, existing, kept] where kept is
// the old first envelope unless the existing password opened it.
[[nodiscard]] SecureBuffer append_and_check(const SecureBuffer &record,
                                            const std::string_view existing,
                                            const std::string_view added,
                                            const std::size_t kept_position) {
  SecureBuffer result = append_card_password_v1(
      record.bytes(), secure_text(existing), secure_text(added));
  expect(result.size() == kCardRecordV1ThreeSlotSize,
         "append did not produce a three-envelope record");
  expect(inspect_card_record(result.bytes()) ==
             CardRecordState::initialized_v1,
         "appended record framing was rejected");
  expect(std::ranges::equal(result.bytes().first(kCardRecordV1HeaderSize),
                            record.bytes().first(kCardRecordV1HeaderSize)),
         "append changed the v1 header");
  expect(std::ranges::equal(envelope_at(result, 2),
                            envelope_at(record, kept_position)),
         "append did not move the kept envelope byte for byte");
  return result;
}

void test_three_slot_framing_keeps_the_v1_header() {
  SecureBuffer initialized =
      initialize_card_record_v1(secure_text("frame-password-1"));
  SecureBuffer appended = append_card_password_v1(
      initialized.bytes(), secure_text("frame-password-1"),
      secure_text("frame-password-2"));
  expect(appended.bytes()[12] == std::byte{216} &&
             appended.bytes()[13] == std::byte{0},
         "three-envelope record changed the v1 length field");

  SecureBuffer relabelled = secure_copy(appended.bytes());
  relabelled.bytes()[12] = std::byte{0x24};
  relabelled.bytes()[13] = std::byte{0x01};
  expect(inspect_card_record(relabelled.bytes()) == CardRecordState::malformed,
         "three-envelope length in the header was accepted");

  SecureBuffer truncated(kCardRecordV1ThreeSlotSize - 1);
  std::ranges::copy(appended.bytes().first(truncated.size()),
                    truncated.bytes().begin());
  expect(inspect_card_record(truncated.bytes()) == CardRecordState::malformed,
         "truncated three-envelope record was accepted");
}

void test_append_follows_v1_rule_with_a_third_slot() {
  auto material = initialize_card_record_and_root_v1(secure_text("slot-pw-1"));
  const SecureBuffer &root = material.root_secret;

  // A single-password v1 card expands in place: [P2, P1, old random second].
  SecureBuffer first = append_and_check(material.record, "slot-pw-1",
                                        "slot-pw-2", 1);
  expect_opens(first, "slot-pw-2", root, "new password did not open");
  expect_opens(first, "slot-pw-1", root, "login password was not kept");

  // Logged in with the newest: the old first envelope is the login, so the
  // second one (P1) is kept. [P3, P2, P1]; all three open.
  SecureBuffer second = append_and_check(first, "slot-pw-2", "slot-pw-3", 1);
  expect_opens(second, "slot-pw-1", root, "moved envelope did not open");
  expect_opens(second, "slot-pw-2", root, "login password was not kept");
  expect_opens(second, "slot-pw-3", root, "new password did not open");

  // Logged in with the oldest: it is kept as current, the most recent other
  // (P3) is kept, and P2 is the one dropped. [P4, P1, P3].
  SecureBuffer third = append_and_check(second, "slot-pw-1", "slot-pw-4", 0);
  expect_opens(third, "slot-pw-1", root, "oldest login password was dropped");
  expect_opens(third, "slot-pw-3", root, "most recent other was dropped");
  expect_opens(third, "slot-pw-4", root, "new password did not open");
  expect_rejected(third, "slot-pw-2", "least recent password survived");

  // [P5, P4, P1]: P3 goes.
  SecureBuffer fourth = append_and_check(third, "slot-pw-4", "slot-pw-5", 1);
  expect_opens(fourth, "slot-pw-1", root, "kept password stopped opening");
  expect_rejected(fourth, "slot-pw-3", "least recent password survived");

  // An unknown duplicate is neither detected nor refused: [P5, P1, P5] has two
  // matching envelopes with the same root and still opens.
  SecureBuffer duplicate = append_and_check(fourth, "slot-pw-1", "slot-pw-5", 0);
  expect_opens(duplicate, "slot-pw-5", root, "duplicate envelopes broke unlock");
  expect_rejected(duplicate, "slot-pw-4", "least recent password survived");

  expect_invalid_argument(
      [&] {
        static_cast<void>(append_card_password_v1(
            fourth.bytes(), secure_text("slot-pw-5"), secure_text("slot-pw-5")));
      },
      "new password equal to the current one was accepted");
  expect_credential_rejected(
      [&] {
        static_cast<void>(append_card_password_v1(
            fourth.bytes(), secure_text("slot-pw-3"), secure_text("slot-pw-6")));
      },
      "a dropped password still authorized an append");
}

void test_two_password_v1_card_expands_keeping_the_other() {
  SecureBuffer initialized =
      initialize_card_record_v1(secure_text("dual-pw-old"));
  SecureBuffer root = unlock_card_record_v1(initialized.bytes(),
                                            secure_text("dual-pw-old"));
  // v1 layout after setting a second password: [newer, older].
  SecureBuffer dual = rewrap_card_record_v1(
      initialized.bytes(), secure_text("dual-pw-old"),
      secure_text("dual-pw-new"), secure_text("dual-pw-old"));

  SecureBuffer from_older = append_and_check(dual, "dual-pw-old",
                                             "dual-pw-third", 0);
  expect_opens(from_older, "dual-pw-new", root, "other v1 password was lost");
  expect_opens(from_older, "dual-pw-old", root, "v1 login password was lost");
  expect_opens(from_older, "dual-pw-third", root, "third password did not open");

  SecureBuffer from_newer = append_and_check(dual, "dual-pw-new",
                                             "dual-pw-third", 1);
  expect_opens(from_newer, "dual-pw-old", root, "other v1 password was lost");
  expect_opens(from_newer, "dual-pw-new", root, "v1 login password was lost");
}

} // namespace

int main() {
  test_public_state_classification_fails_closed();
  test_initialization_and_primary_unlock();
  test_dual_rewrap_accepts_both_credentials_without_shape_change();
  test_invalid_password_input_and_header_versions();
  test_three_slot_framing_keeps_the_v1_header();
  test_append_follows_v1_rule_with_a_third_slot();
  test_two_password_v1_card_expands_keeping_the_other();
  std::cout << "card_record_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
