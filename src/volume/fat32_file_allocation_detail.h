#pragma once
#include "volume/fat32_allocation_plan_detail.h"
#include <span>

namespace usb_vault::volume::detail {
struct Fat32Extent final { std::int64_t next_vcn; std::int64_t lcn; };
class Fat32ExtentCollector final {
 public:
  Fat32ExtentCollector(std::uint64_t expected_clusters, std::uint64_t volume_clusters)
      : expected_(expected_clusters), volume_(volume_clusters) {
    require_geometry(volume_clusters > 0 && volume_clusters < 0x0ffffff5 && expected_clusters <= volume_clusters);
  }
  void consume(std::int64_t start_vcn, std::span<const Fat32Extent> extents) {
    require_geometry(!finished_ && !complete() && start_vcn >= 0 &&
                     static_cast<std::uint64_t>(start_vcn) == next_ && !extents.empty());
    for (const auto& e : extents) {
      require_geometry(e.next_vcn > 0 && e.lcn >= 0);
      const auto end = static_cast<std::uint64_t>(e.next_vcn);
      const auto lcn = static_cast<std::uint64_t>(e.lcn);
      require_geometry(end > next_ && end <= expected_ && lcn < volume_ && end - next_ <= volume_ - lcn);
      const auto count = end - next_;
      if (!runs_.empty() && runs_.back().first_cluster + runs_.back().cluster_count == lcn)
        runs_.back().cluster_count += count;
      else {
        if (runs_.size() == kMaxAllocationPlanRuns)
          throw std::length_error("FAT32 file allocation is too fragmented");
        runs_.push_back({lcn,count});
      }
      next_ = end;
    }
  }
  [[nodiscard]] std::uint64_t next() const noexcept { return next_; }
  [[nodiscard]] bool complete() const noexcept { return next_ == expected_; }
  [[nodiscard]] std::vector<Fat32ClusterRun> finish() {
    require_geometry(!finished_ && complete());
    finished_ = true;
    auto physical = runs_;
    std::sort(physical.begin(),physical.end(),[](const auto& a,const auto& b){return a.first_cluster<b.first_cluster;});
    for(std::size_t i=1;i<physical.size();++i)
      require_geometry(physical[i-1].first_cluster + physical[i-1].cluster_count <= physical[i].first_cluster);
    return std::move(runs_);
  }
 private:
  std::uint64_t expected_{};
  std::uint64_t volume_{};
  std::uint64_t next_{};
  std::vector<Fat32ClusterRun> runs_;
  bool finished_{};
};
}  // namespace usb_vault::volume::detail
