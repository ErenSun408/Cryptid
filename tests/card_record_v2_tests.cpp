#include "usb_vault/card/card_record_v2.h"

#include "card/card_record_crypto.h"
#include "card/card_record_v2_internal.h"
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
#include <utility>

namespace {

using namespace usb_vault::card;
using usb_vault::security::SecureBuffer;
namespace crypto = usb_vault::card::detail;

void expect(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

SecureBuffer text(const std::string_view input) {
  SecureBuffer result(input.size());
  std::ranges::copy(std::as_bytes(std::span(input.data(), input.size())),
      result.bytes().begin());
  return result;
}

SecureBuffer copy(const std::span<const std::byte> input) {
  SecureBuffer result(input.size());
  std::ranges::copy(input, result.bytes().begin());
  return result;
}

bool same(const std::span<const std::byte> a,
    const std::span<const std::byte> b) {
  return std::ranges::equal(a, b);
}

template <typename Error, typename Action>
void rejects(Action&& action, const char* message) {
  try {
    action();
  } catch (const Error&) {
    return;
  }
  throw std::runtime_error(message);
}

auto context(const SecureBuffer& record) {
  return record.bytes().subspan(kCardRecordV2ContextOffset,
      kCardRecordV2ContextSize);
}

auto slot(SecureBuffer& record, const std::size_t index) {
  return record.bytes().subspan(kCardRecordV2EnvelopeOffset +
      index * kCardRecordV2EnvelopeSize, kCardRecordV2EnvelopeSize);
}

void set_slot(SecureBuffer& record, const std::size_t index,
    const SecureBuffer& root, const std::string_view password) {
  crypto::encrypt_card_envelope_v1(context(record),
      static_cast<std::byte>(index), root, text(password), slot(record, index));
}

void write_u32(const std::span<std::byte> bytes, const std::size_t offset,
    const std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

void expect_root(const SecureBuffer& record, const std::string_view password,
    const SecureBuffer& root) {
  auto observed = unlock_card_record_v2(record.bytes(), text(password));
  expect(observed.size() == kCardRecordV2RootSecretSize &&
      same(observed.bytes(), root.bytes()), "v2 returned a different root");
}

void test_public_initialization_and_rejection() {
  auto password = text("v2-initial-password");
  auto initialized = initialize_card_record_and_root_v2(std::move(password));
  expect(password.empty(), "initialization did not consume password ownership");
  expect(initialized.record.size() == 384 &&
      initialized.root_secret.size() == 32, "invalid initialized dimensions");
  expect(inspect_card_record_v2(initialized.record.bytes()) ==
      CardRecordV2State::initialized, "v2 initialization has invalid framing");
  expect(inspect_card_record(initialized.record.bytes()) ==
      CardRecordState::unsupported_version, "v1 accepted a v2 record");
  auto before = copy(initialized.record.bytes());
  expect_root(initialized.record, "v2-initial-password", initialized.root_secret);
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(initialized.record.bytes(),
        text("wrong-v2-password")));
  }, "wrong v2 credential accepted");
  expect(same(before.bytes(), initialized.record.bytes()),
      "authentication mutated input bytes");
  rejects<std::invalid_argument>([] {
    static_cast<void>(initialize_card_record_and_root_v2(SecureBuffer{}));
  }, "empty v2 initialization password accepted");
  const std::array bad_utf8{std::byte{0xc0}, std::byte{0xaf}};
  rejects<std::invalid_argument>([&] {
    static_cast<void>(initialize_card_record_and_root_v2(copy(bad_utf8)));
  }, "invalid UTF-8 initialization accepted");
  rejects<std::invalid_argument>([&] {
    static_cast<void>(unlock_card_record_v2(initialized.record.bytes(),
        copy(bad_utf8)));
  }, "invalid UTF-8 login accepted");
}

void test_all_initial_positions_and_frame_limits() {
  auto authentication_context = crypto::make_card_context_v1();
  SecureBuffer root(32);
  crypto::fill_card_random(root.bytes());
  SecureBuffer first_frame;
  for (std::uint32_t initial_slot = 0; initial_slot < 3; ++initial_slot) {
    auto record = crypto::build_initial_card_record_v2(
        authentication_context.bytes(), root, text("initial-position-password"),
        initial_slot);
    auto result = crypto::authenticate_card_record_v2(record.bytes(),
        text("initial-position-password"));
    expect(same(result.root_secret.bytes(), root.bytes()),
        "initial position changed the root");
    expect(result.next_slot == (initial_slot + 2U) % 3U,
        "initial position has the wrong next slot");
    if (first_frame.empty()) first_frame = copy(record.bytes().first(96));
    expect(same(first_frame.bytes(), record.bytes().first(96)),
        "public frame exposes initial credential position");

    for (std::size_t length = 0; length < kCardRecordV2Size; ++length) {
      expect(inspect_card_record_v2(record.bytes().first(length)) !=
          CardRecordV2State::initialized, "truncated v2 framing accepted");
    }
    SecureBuffer overlong(kCardRecordV2Size + 1);
    std::ranges::copy(record.bytes(), overlong.bytes().begin());
    expect(inspect_card_record_v2(overlong.bytes()) ==
        CardRecordV2State::malformed, "overlong v2 framing accepted");
    for (const std::size_t offset : {0U, 12U, 16U, 31U, 32U, 40U, 44U,
                                    80U, 84U, 88U, 92U}) {
      auto malformed = copy(record.bytes());
      malformed.bytes()[offset] ^= std::byte{1};
      expect(inspect_card_record_v2(malformed.bytes()) !=
          CardRecordV2State::initialized, "malformed v2 header accepted");
      rejects<std::invalid_argument>([&] {
        static_cast<void>(unlock_card_record_v2(malformed.bytes(),
            text("initial-position-password")));
      }, "malformed frame reached authentication");
    }
    auto future = copy(record.bytes());
    future.bytes()[8] = std::byte{3};
    expect(inspect_card_record_v2(future.bytes()) ==
        CardRecordV2State::unsupported_version, "future version not separated");
    rejects<std::invalid_argument>([&] {
      static_cast<void>(unlock_card_record_v2(future.bytes(),
          text("initial-position-password")));
    }, "future version reached authentication");
  }
  rejects<std::invalid_argument>([&] {
    static_cast<void>(crypto::build_initial_card_record_v2(
        authentication_context.bytes(), root, text("bad-position-password"), 3));
  }, "invalid initial slot accepted");
}

void test_upgrade_preserves_unknown_legacy_envelopes() {
  auto old = initialize_card_record_and_root_v1(text("legacy-daily-password"));
  auto single_before = copy(old.record.bytes());
  auto single = upgrade_card_record_v1_to_v2(old.record.bytes(),
      text("legacy-daily-password"));
  expect(same(single.bytes().subspan(32, 64), old.record.bytes().first(64)) &&
      same(single.bytes().subspan(96, 152), old.record.bytes().subspan(64)),
      "single-envelope upgrade rewrote unknown legacy ciphertext");
  expect_root(single, "legacy-daily-password", old.root_secret);
  auto single_state = crypto::authenticate_card_record_v2(single.bytes(),
      text("legacy-daily-password"));
  expect(single_state.next_slot == 2, "single legacy card has wrong first target");
  expect(same(single_before.bytes(), old.record.bytes()),
      "upgrade changed the source legacy record");

  auto dual = rewrap_card_record_v1(old.record.bytes(),
      text("legacy-daily-password"), text("legacy-lure-password"),
      text("legacy-daily-password"));
  auto dual_before = copy(dual.bytes());
  for (const auto credential : {"legacy-daily-password", "legacy-lure-password"}) {
    auto upgraded = upgrade_card_record_v1_to_v2(dual.bytes(), text(credential));
    expect(same(upgraded.bytes().subspan(32, 64), dual.bytes().first(64)) &&
        same(upgraded.bytes().subspan(96, 152), dual.bytes().subspan(64)),
        "dual-envelope upgrade rewrote a preserved legacy envelope");
    expect_root(upgraded, "legacy-daily-password", old.root_secret);
    expect_root(upgraded, "legacy-lure-password", old.root_secret);
    auto state = crypto::authenticate_card_record_v2(upgraded.bytes(),
        text(credential));
    expect(state.next_slot == 2, "dual legacy card has wrong first target");
  }
  expect(same(dual_before.bytes(), dual.bytes()), "dual upgrade mutated input");
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(upgrade_card_record_v1_to_v2(dual.bytes(),
        text("wrong-legacy-password")));
  }, "upgrade accepted an unauthenticated legacy record");
  auto marker = provision_uninitialized_card_record_v1();
  rejects<std::invalid_argument>([&] {
    static_cast<void>(upgrade_card_record_v1_to_v2(marker.bytes(),
        text("legacy-daily-password")));
  }, "upgrade treated a provisioning marker as initialized");
  rejects<std::invalid_argument>([&] {
    static_cast<void>(upgrade_card_record_v1_to_v2(single.bytes(),
        text("legacy-daily-password")));
  }, "upgrade accepted a non-v1 record");
}

void test_three_credentials_and_duplicate_common_roots() {
  auto authentication_context = crypto::make_card_context_v1();
  SecureBuffer root(32);
  crypto::fill_card_random(root.bytes());
  auto record = crypto::build_initial_card_record_v2(authentication_context.bytes(),
      root, text("first-slot-password"), 0);
  set_slot(record, 1, root, "second-slot-password");
  set_slot(record, 2, root, "third-slot-password");
  crypto::seal_card_ring_metadata_v2(record.bytes(), root, 0);
  for (const auto password : {"first-slot-password", "second-slot-password",
                              "third-slot-password"}) {
    expect_root(record, password, root);
  }

  set_slot(record, 0, root, "second-slot-password");
  crypto::seal_card_ring_metadata_v2(record.bytes(), root, 1);
  expect_root(record, "second-slot-password", root);
  set_slot(record, 2, root, "second-slot-password");
  crypto::seal_card_ring_metadata_v2(record.bytes(), root, 2);
  expect_root(record, "second-slot-password", root);
}

void test_multiple_matches_with_different_roots_fail_closed() {
  auto authentication_context = crypto::make_card_context_v1();
  SecureBuffer all_bits(32);
  SecureBuffer different(32);
  std::ranges::fill(all_bits.bytes(), std::byte{0xff});
  std::ranges::fill(different.bytes(), std::byte{1});
  auto record = crypto::build_initial_card_record_v2(authentication_context.bytes(),
      all_bits, text("ambiguous-root-password"), 0);
  set_slot(record, 1, all_bits, "ambiguous-root-password");
  set_slot(record, 2, different, "ambiguous-root-password");
  crypto::seal_card_ring_metadata_v2(record.bytes(), all_bits, 0);
  // OR-selection yields all_bits and therefore valid metadata here. Rejection
  // must come from the explicit pairwise root consistency check.
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(record.bytes(),
        text("ambiguous-root-password")));
  }, "different-root multiple matches accepted despite valid metadata");
  set_slot(record, 1, different, "ambiguous-root-password");
  set_slot(record, 2, all_bits, "ambiguous-root-password");
  crypto::seal_card_ring_metadata_v2(record.bytes(), all_bits, 0);
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(record.bytes(),
        text("ambiguous-root-password")));
  }, "middle-slot different root bypassed consistency check");
}

void test_authenticated_binding_and_metadata_bounds() {
  auto authentication_context = crypto::make_card_context_v1();
  SecureBuffer root(32);
  crypto::fill_card_random(root.bytes());
  auto record = crypto::build_initial_card_record_v2(authentication_context.bytes(),
      root, text("binding-password"), 2);
  for (const std::size_t offset : {48U, 64U, 96U, 172U, 248U, 260U, 308U,
                                  324U, 336U, 383U}) {
    auto altered = copy(record.bytes());
    altered.bytes()[offset] ^= std::byte{1};
    expect(inspect_card_record_v2(altered.bytes()) ==
        CardRecordV2State::initialized, "ciphertext was classified as framing");
    rejects<CardCredentialRejected>([&] {
      static_cast<void>(unlock_card_record_v2(altered.bytes(),
          text("binding-password")));
    }, "tampered context/envelope/metadata accepted");
  }
  auto swapped = copy(record.bytes());
  std::ranges::swap_ranges(slot(swapped, 0), slot(swapped, 2));
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(swapped.bytes(), text("binding-password")));
  }, "envelope slot swap accepted");

  auto key = crypto::derive_card_ring_metadata_key_v2(root, context(record));
  auto opened = crypto::open_card_aead(key, record.bytes().subspan(324, 60),
      record.bytes().first(324));
  expect(opened.authenticated, "fixture metadata failed to authenticate");
  for (const std::uint32_t cursor : {3U, 0xffffffffU}) {
    auto altered = copy(record.bytes());
    auto plaintext = copy(opened.plaintext.bytes());
    write_u32(plaintext.bytes(), 4, cursor);
    crypto::seal_card_aead(key, plaintext.bytes(), altered.bytes().first(324),
        altered.bytes().subspan(324, 60));
    rejects<CardCredentialRejected>([&] {
      static_cast<void>(unlock_card_record_v2(altered.bytes(), text("binding-password")));
    }, "authenticated out-of-range cursor accepted");
  }
  auto altered = copy(record.bytes());
  write_u32(opened.plaintext.bytes(), 0, 2);
  crypto::seal_card_aead(key, opened.plaintext.bytes(), altered.bytes().first(324),
      altered.bytes().subspan(324, 60));
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(altered.bytes(), text("binding-password")));
  }, "authenticated unknown metadata version accepted");

  auto damaged = copy(record.bytes().subspan(324, 60));
  damaged.bytes()[59] ^= std::byte{1};
  auto rejected = crypto::open_card_aead(key, damaged.bytes(), record.bytes().first(324));
  expect(!rejected.authenticated && rejected.plaintext.size() == 32 &&
      std::ranges::all_of(rejected.plaintext.bytes(),
          [](const std::byte value) { return value == std::byte{}; }),
      "failed metadata authentication retained plaintext");
}

} // namespace

int main() {
  try {
    test_public_initialization_and_rejection();
    test_all_initial_positions_and_frame_limits();
    test_upgrade_preserves_unknown_legacy_envelopes();
    test_three_credentials_and_duplicate_common_roots();
    test_multiple_matches_with_different_roots_fail_closed();
    test_authenticated_binding_and_metadata_bounds();
    std::cout << "card_record_v2_tests: PASS (6 scenario groups)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
