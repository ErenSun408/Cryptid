#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace usb_vault::volume {
// Stable MBR or GPT / 512-byte location, not proof of encrypted contents or write authority.
// Keep transient paths, disk numbers and preformat filesystem serials outside.
// External media without a hardware ID are named by their partition table
// instead: the GPT disk GUID, or the MBR disk signature in the first four
// bytes. That flag value never comes from the storage driver.
constexpr std::uint32_t kPartitionTableIdentity = 0x100;
struct PartitionIdentity {
  std::array<std::uint8_t, 16> device_id{};
  std::uint32_t device_id_flags{};
  std::uint32_t layout{1};
  std::uint32_t logical_sector_bytes{512};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::array<std::uint8_t, 16> partition_id{}; // Nonzero GPT GUID; zero for MBR.
  bool operator==(const PartitionIdentity&) const = default;
};
[[nodiscard]] inline bool valid_partition_identity(const PartitionIdentity& p) noexcept {
  return p.device_id != std::array<std::uint8_t, 16>{} &&
      ((p.device_id_flags & ~4U) == 0 || p.device_id_flags == kPartitionTableIdentity) &&
      ((p.layout == 1 && p.partition_id == std::array<std::uint8_t, 16>{}) ||
       (p.layout == 2 && p.partition_id != std::array<std::uint8_t, 16>{})) &&
      p.logical_sector_bytes == 512 && p.offset_bytes != 0 &&
      p.offset_bytes % 512 == 0 && p.length_bytes % 512 == 0 &&
      p.length_bytes >= 20ULL * 1024 * 1024 &&
      p.length_bytes <= 1024ULL * 1024 * 1024 * 1024 &&
      p.offset_bytes <= static_cast<std::uint64_t>(
          (std::numeric_limits<std::int64_t>::max)()) - p.length_bytes;
}
}  // namespace usb_vault::volume
