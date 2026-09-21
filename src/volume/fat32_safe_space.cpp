#include "usb_vault/volume/fat32_safe_space.h"
#include "volume/fat32_safe_space_detail.h"
#include "volume/fat32_allocation_plan_detail.h"

#include "volume/fat32_volume_scan.h"
#include <winioctl.h>
#include <array>
#include <system_error>
#include <vector>

namespace usb_vault::volume {
namespace {
[[noreturn]] void fail(DWORD error) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          "read FAT32 safe-space snapshot");
}
detail::Fat32Geometry read_geometry(HANDLE volume,
                                    std::uint64_t expected_volume_bytes,
                                    std::uint64_t protected_start_in_volume) {
  detail::require_geometry(volume != nullptr && volume != INVALID_HANDLE_VALUE &&
                           expected_volume_bytes != 0 && protected_start_in_volume != 0);
  GET_LENGTH_INFORMATION length{};
  DWORD returned = 0;
  if (!::DeviceIoControl(volume, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &length, sizeof(length), &returned, nullptr)) fail(::GetLastError());
  detail::require_geometry(returned == sizeof(length) && length.Length.QuadPart > 0 &&
      static_cast<std::uint64_t>(length.Length.QuadPart) == expected_volume_bytes);
  std::array<std::byte, 4096> boot{};
  LARGE_INTEGER zero{};
  if (!::SetFilePointerEx(volume, zero, nullptr, FILE_BEGIN)) fail(::GetLastError());
  if (!::ReadFile(volume, boot.data(), static_cast<DWORD>(boot.size()), &returned, nullptr)) fail(::GetLastError());
  detail::require_geometry(returned == boot.size());
  return detail::parse_fat32_geometry(boot, expected_volume_bytes, protected_start_in_volume);
}

template<class FreeCluster>
Fat32SafeSpace scan_bitmap(HANDLE volume, detail::Fat32Geometry geometry,
                          FreeCluster&& free_cluster) {
  DWORD returned = 0;
  detail::Fat32BitmapCounter counter(geometry);
  std::vector<std::byte> buffer(1U << 20);
  while (!counter.complete()) {
    STARTING_LCN_INPUT_BUFFER input{};
    input.StartingLcn.QuadPart = static_cast<LONGLONG>(counter.next());
    const BOOL ok = ::DeviceIoControl(volume, FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input),
        buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
    if (!ok && error != ERROR_MORE_DATA) fail(error);
    constexpr auto header = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    detail::require_geometry(returned > header && returned <= buffer.size());
    const auto* bitmap = reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(buffer.data());
    counter.consume(bitmap->StartingLcn.QuadPart, bitmap->BitmapSize.QuadPart,
                    std::span<const std::byte>(buffer).subspan(header, returned - header), free_cluster);
    detail::require_geometry(ok ? counter.complete() : !counter.complete());
  }
  return {counter.bytes(), geometry.cluster_size};
}
}  // namespace

detail::Fat32Geometry detail::read_fat32_geometry(HANDLE volume, std::uint64_t length, std::uint64_t boundary) {
  return read_geometry(volume, length, boundary);
}
std::uint64_t detail::free_fat32_prefix(HANDLE volume, Fat32Geometry geometry,
                                      std::uint64_t first, std::uint64_t count) {
  require_geometry(first < geometry.safe_clusters && count > 0 && count <= geometry.safe_clusters - first);
  std::uint64_t found = 0;
  bool gap = false;
  static_cast<void>(scan_bitmap(volume, geometry, [&](std::uint64_t cluster) {
    if (cluster < first || found == count || gap) return;
    if (cluster == first + found) ++found;
    else gap = true;
  }));
  return found;
}

Fat32SafeSpace scan_fat32_safe_space(HANDLE volume,
                                   std::uint64_t expected_volume_bytes,
                                   std::uint64_t protected_start_in_volume) {
  return scan_bitmap(volume, read_geometry(volume, expected_volume_bytes,
      protected_start_in_volume), [](std::uint64_t) {});
}

Fat32AllocationPlan plan_fat32_allocation(HANDLE volume,
                                        std::uint64_t expected_volume_bytes,
                                        std::uint64_t protected_start_in_volume,
                                        std::uint64_t file_bytes) {
  const auto geometry = read_geometry(volume, expected_volume_bytes, protected_start_in_volume);
  detail::Fat32FirstFitPlanner planner(geometry, file_bytes);
  // Even after enough clusters are found, validate all bitmap pages before
  // publishing a plan. A truncated final page cannot yield a usable plan.
  static_cast<void>(scan_bitmap(volume, geometry,
      [&](std::uint64_t cluster) { planner.free_cluster(cluster); }));
  return planner.finish();
}
}  // namespace usb_vault::volume
