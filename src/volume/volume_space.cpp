#include "usb_vault/volume/volume_space.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::volume {
namespace {

constexpr DWORD kBitmapBufferBytes = 1U << 20U;

class Handle final {
 public:
  explicit Handle(const HANDLE handle) noexcept : handle_(handle) {}
  ~Handle() noexcept {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(handle_));
    }
  }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[noreturn]] void throw_last_error(const DWORD error, const char* const what) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          what);
}

[[nodiscard]] wchar_t validated_letter(const wchar_t drive_letter) {
  const wchar_t upper = (drive_letter >= L'a' && drive_letter <= L'z')
                            ? static_cast<wchar_t>(drive_letter - L'a' + L'A')
                            : drive_letter;
  if (upper < L'A' || upper > L'Z') {
    throw std::invalid_argument("drive letter must be in the range A-Z");
  }
  return upper;
}

}  // namespace

VolumeSpace volume_space(const wchar_t drive_letter) {
  const wchar_t letter = validated_letter(drive_letter);
  const wchar_t root[] = {letter, L':', L'\\', L'\0'};

  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free_bytes{};
  if (::GetDiskFreeSpaceExW(root, &available, &total, &free_bytes) == FALSE) {
    throw_last_error(::GetLastError(), "query mounted volume free space");
  }
  // What this caller may still write, not what the volume holds in total: on a
  // quota-bearing volume the two differ and the smaller one is the honest answer.
  return {.total_bytes = total.QuadPart, .free_bytes = available.QuadPart};
}

std::uint64_t writable_free_bytes(const std::wstring& directory) {
  if (directory.empty()) {
    throw std::invalid_argument("a save location is required");
  }
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free_bytes{};
  if (::GetDiskFreeSpaceExW(directory.c_str(), &available, &total,
                            &free_bytes) == FALSE) {
    throw_last_error(::GetLastError(), "query save location free space");
  }
  return available.QuadPart;
}

TailSpace scan_volume_tail_space(const wchar_t drive_letter) {
  const wchar_t letter = validated_letter(drive_letter);
  const wchar_t device[] = {L'\\', L'\\', L'.', L'\\', letter, L':', L'\0'};
  const wchar_t root[] = {letter, L':', L'\\', L'\0'};

  DWORD sectors_per_cluster = 0;
  DWORD bytes_per_sector = 0;
  DWORD free_clusters = 0;
  DWORD total_clusters = 0;
  if (::GetDiskFreeSpaceW(root, &sectors_per_cluster, &bytes_per_sector,
                          &free_clusters, &total_clusters) == FALSE ||
      sectors_per_cluster == 0 || bytes_per_sector == 0) {
    throw_last_error(::GetLastError(), "query mounted volume geometry");
  }
  const std::uint64_t cluster_size_64 =
      static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector;
  if (cluster_size_64 > std::numeric_limits<std::uint32_t>::max()) {
    throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                            "invalid cluster size");
  }

  const Handle volume_handle(::CreateFileW(
      device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, 0, nullptr));
  if (volume_handle.get() == INVALID_HANDLE_VALUE) {
    throw_last_error(::GetLastError(),
                     "open volume for allocation bitmap scan");
  }

  std::vector<std::byte> buffer(kBitmapBufferBytes);
  STARTING_LCN_INPUT_BUFFER input{};
  std::uint64_t tail_free_clusters = 0;
  for (;;) {
    DWORD returned = 0;
    const BOOL success = ::DeviceIoControl(
        volume_handle.get(), FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input),
        buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
    const DWORD error = success != FALSE ? ERROR_SUCCESS : ::GetLastError();
    if (success == FALSE && error != ERROR_MORE_DATA) {
      throw_last_error(error, "read filesystem allocation bitmap");
    }
    constexpr std::size_t offset = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    if (returned < offset) {
      throw std::system_error(std::make_error_code(std::errc::protocol_error),
                              "short filesystem allocation bitmap response");
    }
    const auto* bitmap =
        reinterpret_cast<const VOLUME_BITMAP_BUFFER*>(buffer.data());
    const std::uint64_t available_bits =
        static_cast<std::uint64_t>(returned - offset) * 8ULL;
    const std::uint64_t bits = std::min<std::uint64_t>(
        available_bits, static_cast<std::uint64_t>(bitmap->BitmapSize.QuadPart));
    if (bits == 0) {
      throw std::system_error(std::make_error_code(std::errc::protocol_error),
                              "empty filesystem allocation bitmap response");
    }
    // Only the run reaching the end of the volume counts: a hole earlier on is
    // free space the allocator may use, not room before the hidden volume.
    for (std::uint64_t index = 0; index < bits; ++index) {
      const auto byte = std::to_integer<unsigned int>(
          reinterpret_cast<const std::byte*>(bitmap->Buffer)[index >> 3U]);
      const bool used = ((byte >> (index & 7U)) & 1U) != 0;
      tail_free_clusters = used ? 0 : tail_free_clusters + 1;
    }
    if (error != ERROR_MORE_DATA) {
      break;
    }
    input.StartingLcn.QuadPart =
        bitmap->StartingLcn.QuadPart + static_cast<LONGLONG>(bits);
  }

  if (tail_free_clusters >
      std::numeric_limits<std::uint64_t>::max() / cluster_size_64) {
    throw std::system_error(std::make_error_code(std::errc::value_too_large),
                            "filesystem allocation bitmap size overflow");
  }
  return {.bytes = tail_free_clusters * cluster_size_64,
          .cluster_size = static_cast<std::uint32_t>(cluster_size_64)};
}

}  // namespace usb_vault::volume
