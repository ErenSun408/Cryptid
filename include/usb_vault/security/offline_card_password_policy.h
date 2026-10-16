#pragma once

#include "usb_vault/security/confirmed_password.h"

#include <cstddef>

namespace usb_vault::security {

// Length-first baseline for a password that protects a copyable card record
// against offline guessing. Thresholds count well-formed Unicode scalar
// values, not UTF-8 bytes. This policy intentionally does not impose character
// class composition rules or change the exact bytes supplied to the KDF.
class OfflineCardPasswordPolicy final : public IPasswordStrengthPolicy {
 public:
  static constexpr std::size_t minimum_code_points = 6;
  static constexpr std::size_t strong_code_points = 12;
  static constexpr std::size_t maximum_code_points = 128;

  [[nodiscard]] PasswordStrength assess(
      const SecureBuffer& password_utf8) const noexcept override;
};

}  // namespace usb_vault::security
