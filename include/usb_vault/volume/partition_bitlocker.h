#pragma once
#include <cstdint>
namespace usb_vault::volume {
// Windows conversion state: decrypted, encrypted, encrypting, decrypting,
// encryption paused, decryption paused. Unknown results never mean decrypted.
struct PartitionBitLockerStatus final {
  std::uint32_t state{};
  std::uint32_t encrypted_percent{};
};
[[nodiscard]] constexpr bool valid_bitlocker_status(PartitionBitLockerStatus value) noexcept {
  return value.state <= 5 && value.encrypted_percent <= 100 &&
      (value.state != 0 || value.encrypted_percent == 0);
}
} // namespace usb_vault::volume
