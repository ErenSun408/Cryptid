#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstdint>
#include <stdexcept>

namespace usb_vault::security {

// Non-secret, ephemeral feedback from a replaceable password policy. Only the
// two accepted values can be carried by ConfirmedPassword; rejected and
// unknown policy results never reach a card operation.
enum class PasswordStrength : std::uint8_t {
  rejected = 0,
  acceptable = 1,
  strong = 2,
};

class IPasswordStrengthPolicy {
 public:
  virtual ~IPasswordStrengthPolicy() noexcept = default;

  IPasswordStrengthPolicy(const IPasswordStrengthPolicy&) = delete;
  IPasswordStrengthPolicy& operator=(const IPasswordStrengthPolicy&) = delete;
  IPasswordStrengthPolicy(IPasswordStrengthPolicy&&) = delete;
  IPasswordStrengthPolicy& operator=(IPasswordStrengthPolicy&&) = delete;

  // The implementation must not retain, persist, or log the password or any
  // derived feature. It may return only the fixed public feedback enum.
  [[nodiscard]] virtual PasswordStrength assess(
      const SecureBuffer& password_utf8) const noexcept = 0;

 protected:
  IPasswordStrengthPolicy() noexcept = default;
};

enum class PasswordConfirmationRejection : std::uint8_t {
  mismatch = 1,
  insufficient_strength = 2,
};

class PasswordConfirmationRejected final : public std::runtime_error {
 public:
  explicit PasswordConfirmationRejected(PasswordConfirmationRejection reason);

  [[nodiscard]] PasswordConfirmationRejection reason() const noexcept;

 private:
  PasswordConfirmationRejection reason_;
};

// Move-only proof that two independently ingressed password buffers matched
// and the injected policy accepted their value. Raw password bytes can leave
// only by consuming the proof, preventing downstream initialization APIs from
// accidentally accepting an unchecked SecureBuffer.
class ConfirmedPassword final {
 public:
  ConfirmedPassword(const ConfirmedPassword&) = delete;
  ConfirmedPassword& operator=(const ConfirmedPassword&) = delete;
  ConfirmedPassword(ConfirmedPassword&& other) noexcept;
  ConfirmedPassword& operator=(ConfirmedPassword&& other) noexcept;

  [[nodiscard]] PasswordStrength strength() const noexcept;
  [[nodiscard]] SecureBuffer take() &&;

 private:
  friend ConfirmedPassword confirm_password(
      const IPasswordStrengthPolicy&, SecureBuffer, SecureBuffer);

  ConfirmedPassword(SecureBuffer password_utf8,
                    PasswordStrength strength) noexcept;

  SecureBuffer password_utf8_;
  PasswordStrength strength_{PasswordStrength::rejected};
};

// Consumes and clears both independently ingressed buffers on every path.
// Mismatch takes precedence over strength rejection, but the policy is still
// evaluated exactly once so callers cannot accidentally create divergent
// validation paths.
[[nodiscard]] ConfirmedPassword confirm_password(
    const IPasswordStrengthPolicy& policy,
    SecureBuffer password_utf8,
    SecureBuffer confirmation_utf8);

}  // namespace usb_vault::security
