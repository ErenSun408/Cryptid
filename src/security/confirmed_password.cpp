#include "usb_vault/security/confirmed_password.h"

#include <cstddef>
#include <utility>

namespace usb_vault::security {
namespace {

[[nodiscard]] bool equal_bytes(
    const SecureBuffer& left,
    const SecureBuffer& right) noexcept {
  if (left.size() != right.size()) return false;
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(
        left.bytes()[index] ^ right.bytes()[index]);
  }
  return difference == 0;
}

[[nodiscard]] bool accepted_strength(
    const PasswordStrength strength) noexcept {
  return strength == PasswordStrength::acceptable ||
         strength == PasswordStrength::strong;
}

}  // namespace

PasswordConfirmationRejected::PasswordConfirmationRejected(
    const PasswordConfirmationRejection reason)
    : std::runtime_error(
          reason == PasswordConfirmationRejection::mismatch
              ? "password confirmation does not match"
              : "password does not satisfy the strength policy"),
      reason_(reason) {}

PasswordConfirmationRejection
PasswordConfirmationRejected::reason() const noexcept {
  return reason_;
}

ConfirmedPassword::ConfirmedPassword(
    SecureBuffer password_utf8,
    const PasswordStrength strength) noexcept
    : password_utf8_(std::move(password_utf8)), strength_(strength) {}

ConfirmedPassword::ConfirmedPassword(ConfirmedPassword&& other) noexcept
    : password_utf8_(std::move(other.password_utf8_)),
      strength_(std::exchange(other.strength_, PasswordStrength::rejected)) {}

ConfirmedPassword& ConfirmedPassword::operator=(
    ConfirmedPassword&& other) noexcept {
  if (this == &other) return *this;
  password_utf8_ = std::move(other.password_utf8_);
  strength_ = std::exchange(other.strength_, PasswordStrength::rejected);
  return *this;
}

PasswordStrength ConfirmedPassword::strength() const noexcept {
  return strength_;
}

SecureBuffer ConfirmedPassword::take() && {
  if (!accepted_strength(strength_)) {
    throw std::logic_error("confirmed password proof was already consumed");
  }
  strength_ = PasswordStrength::rejected;
  return std::move(password_utf8_);
}

ConfirmedPassword confirm_password(
    const IPasswordStrengthPolicy& policy,
    SecureBuffer password_utf8,
    SecureBuffer confirmation_utf8) {
  const PasswordStrength strength = policy.assess(password_utf8);
  const bool matches = equal_bytes(password_utf8, confirmation_utf8);
  if (!matches) {
    password_utf8.clear();
    confirmation_utf8.clear();
    throw PasswordConfirmationRejected(
        PasswordConfirmationRejection::mismatch);
  }
  if (!accepted_strength(strength)) {
    password_utf8.clear();
    confirmation_utf8.clear();
    throw PasswordConfirmationRejected(
        PasswordConfirmationRejection::insufficient_strength);
  }

  confirmation_utf8.clear();
  return ConfirmedPassword(std::move(password_utf8), strength);
}

}  // namespace usb_vault::security
