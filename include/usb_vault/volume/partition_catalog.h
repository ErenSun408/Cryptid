#pragma once

#include <array>
#include <span>
#include <cstdint>
#include <string>
#include <vector>

namespace usb_vault::volume {
// A candidate is only a read-only discovery result, never write authorization.
enum class PartitionAvailability {
  candidate, windows_volume, metadata_unavailable, unsupported_layout,
  protected_partition, inconsistent_extent, unsupported_sector_size,
  unsupported_filesystem
};
enum class PartitionLayout { unknown, mbr, gpt };
struct PartitionCandidate final {
  std::wstring volume_path;  // Native-only volume GUID path; never a UI input.
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint32_t logical_sector_bytes{};
  PartitionAvailability availability{PartitionAvailability::metadata_unavailable};
  std::uint32_t query_error{};
  PartitionLayout layout{PartitionLayout::unknown};
  std::array<std::uint8_t, 16> partition_id{}; // GPT only; not a hardware identity.
  std::uint32_t filesystem_serial{}; // Detects ordinary reformatting, not cloning.
  bool operator==(const PartitionCandidate&) const = default;
};
// Enumerates volume-backed partitions only, including those without drive letters.
// Missing/protected partitions may not have Windows volume objects. No writes,
// mounting, privilege escalation, password handling or persisted inventory.
// Throws if Windows-volume identification or the enumeration itself fails.
[[nodiscard]] std::vector<PartitionCandidate> enumerate_partition_candidates();
// Requires exactly one unchanged, eligible native snapshot. This is selection
// freshness only, never a retained handle, hardware identity or write permit.
[[nodiscard]] bool matches_partition_snapshot(const PartitionCandidate& selected,
    std::span<const PartitionCandidate> current) noexcept;
}  // namespace usb_vault::volume
