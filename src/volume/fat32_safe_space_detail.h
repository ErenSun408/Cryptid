#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace usb_vault::volume::detail {
struct Fat32Geometry final {
  std::uint64_t data_offset{};
  std::uint32_t cluster_size{};
  std::uint64_t cluster_count{};
  std::uint64_t safe_clusters{};
};

inline void require_geometry(bool valid) {
  if (!valid) throw std::invalid_argument("invalid FAT32 safe-space geometry");
}
inline std::uint32_t little16(std::span<const std::byte> b, std::size_t n) {
  return std::to_integer<std::uint32_t>(b[n]) |
         (std::to_integer<std::uint32_t>(b[n + 1]) << 8);
}
inline std::uint32_t little32(std::span<const std::byte> b, std::size_t n) {
  return little16(b, n) | (little16(b, n + 2) << 16);
}
inline Fat32Geometry parse_fat32_geometry(std::span<const std::byte> boot,
                                         std::uint64_t volume_bytes,
                                         std::uint64_t boundary) {
  require_geometry(boot.size() >= 512 && little16(boot, 510) == 0xaa55);
  const auto sector = little16(boot, 11);
  const auto spc = std::to_integer<std::uint32_t>(boot[13]);
  const auto reserved = little16(boot, 14);
  const auto fats = std::to_integer<std::uint32_t>(boot[16]);
  const std::uint64_t fat_sectors = little32(boot, 36);
  const std::uint64_t sectors = little32(boot, 32);
  require_geometry((sector == 512 || sector == 1024 || sector == 2048 || sector == 4096) &&
                   spc != 0 && spc <= 128 && (spc & (spc - 1)) == 0 && reserved != 0 &&
                   (fats == 1 || fats == 2) && fat_sectors != 0 && sectors != 0 &&
                   little16(boot, 17) == 0 && little16(boot, 19) == 0 &&
                   little16(boot, 22) == 0 && little16(boot, 42) == 0);
  const std::uint64_t data_sectors = reserved + fats * fat_sectors;
  require_geometry(data_sectors < sectors && sectors * sector <= volume_bytes &&
                   boundary <= sectors * sector && boundary > data_sectors * sector);
  const auto clusters = (sectors - data_sectors) / spc;
  const auto root = little32(boot, 44);
  require_geometry(clusters >= 65525 && clusters < 0x0ffffff5 &&
                   fat_sectors * sector / 4 >= clusters + 2 &&
                   root >= 2 && root < clusters + 2);
  const auto cluster_size = sector * spc;
  return {data_sectors * sector, cluster_size, clusters,
          std::min(clusters, (boundary - data_sectors * sector) / cluster_size)};
}

// Allows Windows to round StartingLcn down; overlap is counted only once.
// Gaps, short frames and nonprogress must fail rather than overstate capacity.
class Fat32BitmapCounter final {
 public:
  explicit Fat32BitmapCounter(Fat32Geometry geometry) : geometry_(geometry) {
    require_geometry(geometry.cluster_count > 0 && geometry.cluster_count < 0x0ffffff5 &&
                     geometry.safe_clusters <= geometry.cluster_count &&
                     geometry.cluster_size > 0 && geometry.cluster_size <= 524288);
  }
  void consume(std::int64_t start, std::int64_t reported_bits,
               std::span<const std::byte> payload) {
    consume(start, reported_bits, payload, [](std::uint64_t) {});
  }
  template<class FreeCluster>
  void consume(std::int64_t start, std::int64_t reported_bits,
               std::span<const std::byte> payload, FreeCluster&& free_cluster) {
    require_geometry(start >= 0 && reported_bits > 0 && !payload.empty());
    const auto first = static_cast<std::uint64_t>(start);
    require_geometry(first <= next_ && first < geometry_.cluster_count &&
                     static_cast<std::uint64_t>(reported_bits) >= geometry_.cluster_count - first);
    const auto bits = std::min<std::uint64_t>(geometry_.cluster_count - first,
                                            payload.size() * 8ULL);
    require_geometry(bits > next_ - first);
    const auto end = first + bits;
    for (auto cluster = next_; cluster < std::min(end, geometry_.safe_clusters); ++cluster) {
      const auto index = cluster - first;
      if ((std::to_integer<unsigned int>(payload[static_cast<std::size_t>(index / 8)]) &
           (1U << (index % 8))) == 0) {
        free_cluster(cluster);
        ++free_;
      }
    }
    next_ = end;
  }
  [[nodiscard]] std::uint64_t next() const noexcept { return next_; }
  [[nodiscard]] bool complete() const noexcept { return next_ == geometry_.cluster_count; }
  [[nodiscard]] std::uint64_t bytes() const {
    require_geometry(complete());
    return free_ * geometry_.cluster_size;
  }
 private:
  Fat32Geometry geometry_;
  std::uint64_t next_{};
  std::uint64_t free_{};
};
}  // namespace usb_vault::volume::detail
