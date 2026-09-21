#pragma once

#include "usb_vault/volume/fat32_safe_space.h"
#include "volume/fat32_safe_space_detail.h"
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace usb_vault::volume::detail {
inline constexpr std::size_t kMaxAllocationPlanRuns = 65536;

class Fat32FirstFitPlanner final {
 public:
  Fat32FirstFitPlanner(Fat32Geometry geometry, std::uint64_t file_bytes)
      : safe_clusters_(geometry.safe_clusters) {
    require_geometry(geometry.cluster_size >= 512 && geometry.cluster_size <= 524288 &&
        (geometry.cluster_size & (geometry.cluster_size - 1)) == 0 &&
        geometry.safe_clusters <= geometry.cluster_count && geometry.cluster_count < 0x0ffffff5);
    if (file_bytes > std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("file exceeds FAT32 size limit");
    remaining_ = file_bytes / geometry.cluster_size + (file_bytes % geometry.cluster_size != 0);
    plan_.file_bytes = file_bytes;
    plan_.cluster_size = geometry.cluster_size;
    plan_.allocation_bytes = remaining_ * geometry.cluster_size;
  }

  void free_cluster(std::uint64_t cluster) {
    require_geometry(!finished_ && cluster < safe_clusters_ && (!last_ || cluster > *last_));
    last_ = cluster;
    if (remaining_ == 0) return;
    if (!plan_.runs.empty() &&
        plan_.runs.back().first_cluster + plan_.runs.back().cluster_count == cluster) {
      ++plan_.runs.back().cluster_count;
    } else {
      if (plan_.runs.size() == kMaxAllocationPlanRuns)
        throw std::length_error("FAT32 allocation plan is too fragmented");
      plan_.runs.push_back({cluster, 1});
    }
    --remaining_;
  }

  [[nodiscard]] Fat32AllocationPlan finish() {
    require_geometry(!finished_);
    finished_ = true;
    if (remaining_ != 0)
      throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                              "insufficient protected FAT32 space");
    return std::move(plan_);
  }
 private:
  Fat32AllocationPlan plan_;
  std::uint64_t safe_clusters_{};
  std::uint64_t remaining_{};
  std::optional<std::uint64_t> last_;
  bool finished_{};
};
}  // namespace usb_vault::volume::detail
