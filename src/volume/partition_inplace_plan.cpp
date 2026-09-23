#include "usb_vault/volume/partition_inplace_plan.h"
#include "volume/partition_inplace_plan_detail.h"
#include "volume/partition_catalog_detail.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <winioctl.h>

namespace usb_vault::volume {
namespace {
constexpr std::uint64_t kSector = 512;
constexpr std::uint64_t kHeaderGroup = 128ULL * 1024;
constexpr std::uint64_t kHeaders = 2 * kHeaderGroup;
constexpr std::uint64_t kMaxChunk = 2ULL * 1024 * 1024;
void check_size(std::uint64_t bytes) {
  if (bytes < 20ULL * 1024 * 1024 || bytes > 1024ULL * 1024 * 1024 * 1024 || bytes % kSector)
    throw std::invalid_argument("unsupported in-place partition size");
}
}
PartitionInplacePlan plan_partition_inplace(const std::uint64_t partition_bytes,
    const NtfsInplaceFacts& facts) {
  check_size(partition_bytes);
  if (facts.sector_bytes != kSector || facts.cluster_bytes < kSector ||
      facts.cluster_bytes > 65536 || !std::has_single_bit(facts.cluster_bytes) ||
      facts.sectors <= 0 || facts.clusters <= 0 || facts.free_clusters < 0 ||
      facts.free_clusters > facts.clusters)
    throw std::invalid_argument("unsupported NTFS geometry");
  const auto sectors = static_cast<std::uint64_t>(facts.sectors);
  const auto clusters = static_cast<std::uint64_t>(facts.clusters);
  const auto free_clusters = static_cast<std::uint64_t>(facts.free_clusters);
  // Divide before multiplying untrusted counters; NTFS may omit its final
  // backup boot sector from NumberSectors, hence the partial final cluster.
  if (sectors > partition_bytes / kSector)
    throw std::invalid_argument("NTFS exceeds partition");
  const auto ntfs_bytes = sectors * kSector;
  if (ntfs_bytes <= kHeaders + 884ULL * 4096 ||
      clusters < ntfs_bytes / facts.cluster_bytes ||
      clusters > (ntfs_bytes + facts.cluster_bytes - 1) / facts.cluster_bytes ||
      free_clusters < kHeaders / facts.cluster_bytes ||
      (clusters - free_clusters) > (ntfs_bytes - kHeaders) / facts.cluster_bytes)
    throw std::invalid_argument("NTFS lacks header reservation space");
  return {.partition_bytes = partition_bytes, .ntfs_bytes = ntfs_bytes,
      .shrink_to_sectors = (ntfs_bytes - kHeaders) / kSector,
      .data_offset_bytes = kHeaderGroup, .data_bytes = partition_bytes - kHeaders,
      .backup_header_offset_bytes = partition_bytes - kHeaderGroup};
}
std::optional<PartitionInplaceChunk> next_partition_inplace_chunk(
    const std::uint64_t partition_bytes, const std::uint64_t start,
    const std::uint64_t length) {
  check_size(partition_bytes);
  const auto end = partition_bytes - kHeaderGroup;
  // Avoid start+length overflow; require a contiguous encrypted suffix.
  if (start < kHeaderGroup || start > end || start % kSector || length % kSector || length != end - start)
    throw std::invalid_argument("invalid in-place encrypted range");
  const auto remaining = start - kHeaderGroup;
  if (!remaining) return std::nullopt; // Data done; header finalization is separate.
  const auto bytes = (std::min)(remaining, kMaxChunk);
  const auto destination = start - bytes;
  return PartitionInplaceChunk{.source_offset_bytes = destination - kHeaderGroup,
      .destination_offset_bytes = destination, .bytes = static_cast<std::uint32_t>(bytes),
      .encryption_unit = destination / kSector};
}
PartitionInplacePlan detail::inspect_partition_inplace(HANDLE handle,
    const PartitionCandidate& expected) {
  if (expected.layout != PartitionLayout::mbr || expected.logical_sector_bytes != kSector ||
      !matches_partition_handle_identity(handle, expected))
    throw std::invalid_argument("in-place target identity rejected");
  DWORD returned{}, dirty{};
  NTFS_VOLUME_DATA_BUFFER ntfs{};
  if (!::DeviceIoControl(handle, FSCTL_IS_VOLUME_DIRTY, nullptr, 0, &dirty, sizeof(dirty), &returned, nullptr) ||
      returned != sizeof(dirty) || dirty != 0)
    throw std::runtime_error("NTFS dirty status unknown or requires repair");
  if (!::DeviceIoControl(handle, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &ntfs, sizeof(ntfs), &returned, nullptr) ||
      returned < sizeof(ntfs))
    throw std::runtime_error("NTFS geometry unavailable");
  const auto plan = plan_partition_inplace(expected.length_bytes,
      {.sectors = ntfs.NumberSectors.QuadPart, .clusters = ntfs.TotalClusters.QuadPart,
       .free_clusters = ntfs.FreeClusters.QuadPart, .sector_bytes = ntfs.BytesPerSector,
       .cluster_bytes = ntfs.BytesPerCluster});
  if (!matches_partition_handle_identity(handle, expected))
    throw std::runtime_error("in-place target changed during inspection");
  return plan;
}
}  // namespace usb_vault::volume
