#pragma once
#include "usb_vault/volume/fat32_safe_space.h"

namespace usb_vault::volume {
// Read-only allocation inspection on the caller's exclusively owned file handle.
// Geometry must belong to the already validated FAT32 volume containing it.
// Rejects sparse, overlapping, incomplete or out-of-volume mappings, as well as
// changed file sizes. Runs are returned in logical file order, not LCN order.
[[nodiscard]] std::vector<Fat32ClusterRun> read_fat32_file_allocation(
    HANDLE file, std::uint64_t expected_file_bytes,
    std::uint32_t cluster_size, std::uint64_t volume_clusters);
}  // namespace usb_vault::volume
