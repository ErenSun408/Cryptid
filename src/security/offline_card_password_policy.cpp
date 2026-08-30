#include "usb_vault/security/offline_card_password_policy.h"

#include <cstddef>
#include <cstdint>

namespace usb_vault::security {
namespace {

[[nodiscard]] std::uint8_t octet(const std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool is_continuation(const std::uint8_t value) noexcept {
  return value >= 0x80U && value <= 0xBFU;
}

[[nodiscard]] bool has_bytes(const std::size_t index,
                             const std::size_t count,
                             const std::size_t size) noexcept {
  return index <= size && count <= size - index;
}

[[nodiscard]] std::size_t sequence_length(
    const std::span<const std::byte> bytes,
    const std::size_t index) noexcept {
  const std::uint8_t first = octet(bytes[index]);
  if (first <= 0x7FU) return 1;

  if (first >= 0xC2U && first <= 0xDFU) {
    if (!has_bytes(index, 2, bytes.size()) ||
        !is_continuation(octet(bytes[index + 1]))) {
      return 0;
    }
    return 2;
  }

  if (first >= 0xE0U && first <= 0xEFU) {
    if (!has_bytes(index, 3, bytes.size())) return 0;
    const std::uint8_t second = octet(bytes[index + 1]);
    const std::uint8_t third = octet(bytes[index + 2]);
    const bool valid_second =
        first == 0xE0U ? second >= 0xA0U && second <= 0xBFU
                       : first == 0xEDU
                             ? second >= 0x80U && second <= 0x9FU
                             : is_continuation(second);
    return valid_second && is_continuation(third) ? 3 : 0;
  }

  if (first >= 0xF0U && first <= 0xF4U) {
    if (!has_bytes(index, 4, bytes.size())) return 0;
    const std::uint8_t second = octet(bytes[index + 1]);
    const std::uint8_t third = octet(bytes[index + 2]);
    const std::uint8_t fourth = octet(bytes[index + 3]);
    const bool valid_second =
        first == 0xF0U ? second >= 0x90U && second <= 0xBFU
                       : first == 0xF4U
                             ? second >= 0x80U && second <= 0x8FU
                             : is_continuation(second);
    return valid_second && is_continuation(third) &&
                   is_continuation(fourth)
               ? 4
               : 0;
  }

  return 0;
}

}  // namespace

PasswordStrength OfflineCardPasswordPolicy::assess(
    const SecureBuffer& password_utf8) const noexcept {
  std::size_t code_points = 0;
  std::size_t index = 0;
  while (index < password_utf8.size()) {
    const std::size_t length = sequence_length(password_utf8.bytes(), index);
    if (length == 0) return PasswordStrength::rejected;
    index += length;
    ++code_points;
    if (code_points > maximum_code_points) {
      return PasswordStrength::rejected;
    }
  }

  if (code_points < minimum_code_points) {
    return PasswordStrength::rejected;
  }
  return code_points >= strong_code_points ? PasswordStrength::strong
                                           : PasswordStrength::acceptable;
}

}  // namespace usb_vault::security
