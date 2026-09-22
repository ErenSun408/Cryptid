#pragma once

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
struct PartitionCandidate final {
  std::wstring volume_path;  // Native-only volume GUID path; never a UI input.
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint32_t logical_sector_bytes{};
  PartitionAvailability availability{PartitionAvailability::metadata_unavailable};
  std::uint32_t query_error{};
};
// Enumerates volume-backed partitions only, including those without drive letters.
// Missing/protected partitions may not have Windows volume objects. No writes,
// mounting, privilege escalation, password handling or persisted inventory.
// Throws if Windows-volume identification or the enumeration itself fails.
[[nodiscard]] std::vector<PartitionCandidate> enumerate_partition_candidates();
}  // namespace usb_vault::volume
