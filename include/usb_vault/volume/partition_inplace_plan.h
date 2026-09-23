#pragma once
#include <cstdint>
#include <optional>

namespace usb_vault::volume {
// Arithmetic only: no authorization to shrink, move clusters or write headers.
// Matches the pinned 1.26.29 ordinary-volume layout, restricted to MBR/512.
struct NtfsInplaceFacts final {
  std::int64_t sectors{};
  std::int64_t clusters{};
  std::int64_t free_clusters{};
  std::uint32_t sector_bytes{};
  std::uint32_t cluster_bytes{};
};
struct PartitionInplacePlan final {
  std::uint64_t partition_bytes{};
  std::uint64_t ntfs_bytes{};
  std::uint64_t shrink_to_sectors{};
  std::uint64_t data_offset_bytes{};
  std::uint64_t data_bytes{};
  std::uint64_t backup_header_offset_bytes{};
  bool operator==(const PartitionInplacePlan&) const = default;
};
[[nodiscard]] PartitionInplacePlan plan_partition_inplace(
    std::uint64_t partition_bytes, const NtfsInplaceFacts& facts);
struct PartitionInplaceChunk final {
  std::uint64_t source_offset_bytes{};
  std::uint64_t destination_offset_bytes{};
  std::uint32_t bytes{};
  std::uint64_t encryption_unit{};
};
// Accept range facts only after authenticating the backup header and binding it
// to this device. Arithmetic validity does not authenticate a paused operation.
// The whole source chunk must be read before its overlapping destination write.
// A caller advances the range only after a successful full write. No IO here.
[[nodiscard]] std::optional<PartitionInplaceChunk> next_partition_inplace_chunk(
    std::uint64_t partition_bytes, std::uint64_t encrypted_area_start,
    std::uint64_t encrypted_area_length);
}  // namespace usb_vault::volume
