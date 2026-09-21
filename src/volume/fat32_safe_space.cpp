#include "usb_vault/volume/fat32_safe_space.h"
#include "volume/fat32_safe_space_detail.h"

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
}
Fat32SafeSpace scan_fat32_safe_space(HANDLE volume,
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
  const auto geometry = detail::parse_fat32_geometry(boot, expected_volume_bytes,
                                                    protected_start_in_volume);
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
                    std::span<const std::byte>(buffer).subspan(header, returned - header));
    detail::require_geometry(ok ? counter.complete() : !counter.complete());
  }
  return {counter.bytes(), geometry.cluster_size};
}
}  // namespace usb_vault::volume
