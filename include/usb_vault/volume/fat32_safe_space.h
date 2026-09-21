#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>

namespace usb_vault::volume {
struct Fat32SafeSpace final {
  std::uint64_t free_bytes{};
  std::uint32_t cluster_size{};
};

// Read-only scan on a caller-owned, exclusively used volume handle. Both byte
// values must come from authenticated geometry for this same mounted volume.
// Returns a snapshot of whole free clusters before the protected boundary,
// never a guarantee of file payload capacity. Does not open paths or log data.
[[nodiscard]] Fat32SafeSpace scan_fat32_safe_space(
    HANDLE volume, std::uint64_t expected_volume_bytes,
    std::uint64_t protected_start_in_volume);
struct Fat32ClusterRun final {
  std::uint64_t first_cluster{};
  std::uint64_t cluster_count{};
  friend bool operator==(const Fat32ClusterRun&, const Fat32ClusterRun&) = default;
};

struct Fat32AllocationPlan final {
  std::uint64_t file_bytes{};
  std::uint64_t allocation_bytes{};
  std::uint32_t cluster_size{};
  std::vector<Fat32ClusterRun> runs;
};

// Read-only first-fit snapshot, in ascending cluster order. No target is created
// and no clusters are reserved. A later writer must revalidate each destination
// and the actual allocation before writing. Layouts remain memory-only.
// FAT32 files are limited to UINT32_MAX bytes. Extreme fragmentation exceeding
// 65,536 planned runs fails closed rather than allocating unbounded memory.
[[nodiscard]] Fat32AllocationPlan plan_fat32_allocation(
    HANDLE volume, std::uint64_t expected_volume_bytes,
    std::uint64_t protected_start_in_volume, std::uint64_t file_bytes);
}  // namespace usb_vault::volume
