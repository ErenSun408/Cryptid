#include "card/card_record_crypto.h"
#include "card/card_record_v2_internal.h"
#include "usb_vault/card/card_record_v2.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace {

using namespace usb_vault::card;
using usb_vault::security::SecureBuffer;
namespace crypto = usb_vault::card::detail;
constexpr std::array<std::string_view, 5> kPasswords{
    "ring-password-one", "ring-password-two", "ring-password-three",
    "ring-password-four", "ring-password-five"};

void expect(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

SecureBuffer copy(const std::span<const std::byte> input) {
  SecureBuffer result(input.size());
  std::ranges::copy(input, result.bytes().begin());
  return result;
}

SecureBuffer text(const std::string_view input) {
  return copy(std::as_bytes(std::span(input.data(), input.size())));
}

bool same(const std::span<const std::byte> a, const std::span<const std::byte> b) {
  return std::ranges::equal(a, b);
}

template <typename Error, typename Action>
void rejects(Action&& action, const char* message) {
  try { action(); }
  catch (const Error&) { return; }
  throw std::runtime_error(message);
}

void expect_password(const SecureBuffer& record, const std::string_view password,
    const SecureBuffer& root) {
  auto found = unlock_card_record_v2(record.bytes(), text(password));
  expect(same(found.bytes(), root.bytes()), "ring password changed root");
}

void test_circular_append_for_every_initial_position() {
  auto context = crypto::make_card_context_v1();
  SecureBuffer root(32);
  crypto::fill_card_random(root.bytes());
  for (std::uint32_t start = 0; start < 3; ++start) {
    auto record = crypto::build_initial_card_record_v2(context.bytes(), root,
        text(kPasswords[0]), start);
    for (std::size_t added = 1; added < kPasswords.size(); ++added) {
      auto before = copy(record.bytes());
      auto state = crypto::authenticate_card_record_v2(before.bytes(),
          text(kPasswords[added - 1]));
      // P4 intentionally authenticates using P1, the very slot being replaced.
      auto current = added == 3 ? kPasswords[0] : kPasswords[added - 1];
      record = append_card_password_v2(before.bytes(), text(current), text(kPasswords[added]));
      expect(same(before.bytes().first(96), record.bytes().first(96)),
          "append changed immutable context");
      for (std::size_t slot = 0; slot < 3; ++slot) {
        expect(same(before.bytes().subspan(96 + slot * 76, 76),
            record.bytes().subspan(96 + slot * 76, 76)) == (slot != state.next_slot),
            "append rewrote a retained slot or missed its target");
      }
      const auto oldest = added >= 3 ? added - 2 : 0;
      for (std::size_t i = oldest; i <= added; ++i) {
        expect_password(record, kPasswords[i], root);
      }
      if (added >= 3) {
        rejects<CardCredentialRejected>([&] {
          static_cast<void>(unlock_card_record_v2(record.bytes(), text(kPasswords[oldest - 1])));
        }, "circular append retained the overwritten credential");
      }
    }
  }
}

void test_unknown_duplicate_and_invalid_append() {
  auto initial = initialize_card_record_and_root_v2(text(kPasswords[0]));
  auto two = append_card_password_v2(initial.record.bytes(), text(kPasswords[0]), text(kPasswords[1]));
  auto state = crypto::authenticate_card_record_v2(two.bytes(), text(kPasswords[1]));
  auto repeated = append_card_password_v2(two.bytes(), text(kPasswords[1]), text(kPasswords[0]));
  auto after = crypto::authenticate_card_record_v2(repeated.bytes(), text(kPasswords[0]));
  expect(after.next_slot == crypto::next_card_slot_v2(state.next_slot),
      "unknown duplicate did not consume a ring position");
  expect_password(repeated, kPasswords[0], initial.root_secret);
  expect_password(repeated, kPasswords[1], initial.root_secret);
  auto before = copy(repeated.bytes());
  rejects<std::invalid_argument>([&] {
    static_cast<void>(append_card_password_v2(repeated.bytes(), text(kPasswords[1]), text(kPasswords[1])));
  }, "known current password was accepted as a new password");
  rejects<std::invalid_argument>([&] {
    static_cast<void>(append_card_password_v2(repeated.bytes(), text(kPasswords[1]), SecureBuffer{}));
  }, "empty append password accepted");
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(append_card_password_v2(repeated.bytes(), text("wrong-current-password"), text(kPasswords[2])));
  }, "append accepted an unauthenticated current password");
  expect(same(before.bytes(), repeated.bytes()), "failed append changed its source");
}

} // namespace

int main() {
  try {
    test_circular_append_for_every_initial_position();
    test_unknown_duplicate_and_invalid_append();
    std::cout << "card_record_v2_append_tests: PASS (2 scenario groups)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
