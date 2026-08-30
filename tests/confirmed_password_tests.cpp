#include "usb_vault/security/confirmed_password.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

using usb_vault::security::ConfirmedPassword;
using usb_vault::security::IPasswordStrengthPolicy;
using usb_vault::security::PasswordConfirmationRejected;
using usb_vault::security::PasswordConfirmationRejection;
using usb_vault::security::PasswordStrength;
using usb_vault::security::SecureBuffer;
using usb_vault::security::confirm_password;

static_assert(!std::is_default_constructible_v<ConfirmedPassword>);
static_assert(!std::is_copy_constructible_v<ConfirmedPassword>);
static_assert(!std::is_copy_assignable_v<ConfirmedPassword>);
static_assert(std::is_nothrow_move_constructible_v<ConfirmedPassword>);

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

class FixedPolicy final : public IPasswordStrengthPolicy {
 public:
  [[nodiscard]] PasswordStrength assess(
      const SecureBuffer& password_utf8) const noexcept override {
    ++calls;
    observed_size = password_utf8.size();
    observed_zero = password_utf8.size() > 1 &&
                    password_utf8.bytes()[1] == std::byte{};
    return result;
  }

  PasswordStrength result{PasswordStrength::acceptable};
  mutable std::size_t calls{};
  mutable std::size_t observed_size{};
  mutable bool observed_zero{};
};

template <typename Action>
void require_rejection(Action&& action,
                       const PasswordConfirmationRejection expected,
                       const char* const message) {
  try {
    action();
  } catch (const PasswordConfirmationRejected& error) {
    require(error.reason() == expected, message);
    return;
  }
  throw std::runtime_error(message);
}

void test_matching_password_creates_move_only_proof() {
  FixedPolicy policy;
  policy.result = PasswordStrength::strong;
  const std::string_view bytes{"a\0\xC3\xA4", 4};
  ConfirmedPassword confirmed = confirm_password(
      policy, secure_text(bytes), secure_text(bytes));

  require(policy.calls == 1 && policy.observed_size == bytes.size() &&
              policy.observed_zero,
          "strength policy did not receive the exact primary bytes once");
  require(confirmed.strength() == PasswordStrength::strong,
          "confirmed password lost strength feedback");
  SecureBuffer released = std::move(confirmed).take();
  require(released.size() == bytes.size() &&
              std::ranges::equal(released.bytes(),
                                 std::as_bytes(std::span(bytes))),
          "confirmed password changed released bytes");
}

void test_moved_from_proof_is_invalidated() {
  FixedPolicy policy;
  ConfirmedPassword original = confirm_password(
      policy, secure_text("move-proof"), secure_text("move-proof"));
  ConfirmedPassword moved(std::move(original));
  require(original.strength() == PasswordStrength::rejected,
          "move construction left the source proof accepted");
  try {
    static_cast<void>(std::move(original).take());
    throw std::runtime_error("moved-from proof released a password");
  } catch (const std::logic_error&) {
  }
  SecureBuffer released = std::move(moved).take();
  require(released.size() == std::string_view("move-proof").size(),
          "move construction lost the valid proof password");
}

void test_mismatch_precedes_strength_rejection() {
  FixedPolicy policy;
  policy.result = PasswordStrength::rejected;
  require_rejection(
      [&] {
        static_cast<void>(confirm_password(
            policy, secure_text("first"), secure_text("other")));
      },
      PasswordConfirmationRejection::mismatch,
      "same-size mismatch returned the wrong rejection");
  require(policy.calls == 1,
          "mismatched password skipped or repeated strength assessment");

  require_rejection(
      [&] {
        static_cast<void>(confirm_password(
            policy, secure_text("short"), secure_text("longer")));
      },
      PasswordConfirmationRejection::mismatch,
      "different-size mismatch returned the wrong rejection");
  require(policy.calls == 2,
          "different-size mismatch used a divergent policy path");
}

void test_rejected_and_unknown_strength_never_create_proof() {
  for (const PasswordStrength strength : {
           PasswordStrength::rejected,
           static_cast<PasswordStrength>(255)}) {
    FixedPolicy policy;
    policy.result = strength;
    require_rejection(
        [&] {
          static_cast<void>(confirm_password(
              policy, secure_text("matching"), secure_text("matching")));
        },
        PasswordConfirmationRejection::insufficient_strength,
        "unaccepted strength created a confirmed password");
    require(policy.calls == 1,
            "unaccepted strength was not assessed exactly once");
  }
}

}  // namespace

int main() {
  try {
    test_matching_password_creates_move_only_proof();
    test_moved_from_proof_is_invalidated();
    test_mismatch_precedes_strength_rejection();
    test_rejected_and_unknown_strength_never_create_proof();
    std::cout << "confirmed_password_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "confirmed_password_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
