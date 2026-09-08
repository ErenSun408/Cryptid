#include "usb_vault/volume/volume_directory_listing.h"

#include "volume/volume_containment.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace usb_vault::volume {
namespace {

// 100-nanosecond ticks between 1601-01-01 and 1970-01-01.
constexpr std::int64_t kTicksToUnixEpoch = 116444736000000000LL;
constexpr std::int64_t kTicksPerMillisecond = 10000LL;

[[noreturn]] void throw_last_error(const DWORD error,
                                   const char* const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

[[nodiscard]] std::int64_t to_unix_milliseconds(const FILETIME& time) noexcept {
  const std::int64_t ticks =
      (static_cast<std::int64_t>(time.dwHighDateTime) << 32) |
      static_cast<std::int64_t>(time.dwLowDateTime);
  return (ticks - kTicksToUnixEpoch) / kTicksPerMillisecond;
}

class FindHandle final {
 public:
  explicit FindHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FindHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::FindClose(handle_);
  }
  FindHandle(const FindHandle&) = delete;
  FindHandle& operator=(const FindHandle&) = delete;
  FindHandle(FindHandle&&) = delete;
  FindHandle& operator=(FindHandle&&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[nodiscard]] bool is_dot_entry(const wchar_t* const name) noexcept {
  return name[0] == L'.' &&
         (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

// Directories first, then ordinal case-insensitive order. Ordinal rather than
// locale-aware because the order has to be the same on every machine: a
// listing that reorders itself with the user's locale would make the page's
// selection indexes mean different entries in different places.
[[nodiscard]] bool orders_before(const VolumeDirectoryEntry& left,
                                 const VolumeDirectoryEntry& right) noexcept {
  if (left.is_directory != right.is_directory) return left.is_directory;
  const int comparison = ::CompareStringOrdinal(
      left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
  if (comparison == CSTR_EQUAL || comparison == 0) {
    // Two names equal but for case cannot both exist on one Windows volume,
    // so this only settles a tie the file system will never produce. Falling
    // back to the case-sensitive order keeps the result total either way.
    return left.name < right.name;
  }
  return comparison == CSTR_LESS_THAN;
}

}  // namespace

VolumeDirectoryListing list_volume_directory(
    const std::wstring_view absolute_directory) {
  const wchar_t drive = require_drive_absolute(absolute_directory);
  std::wstring directory(absolute_directory);
  require_directory_stays_on_volume(directory, drive);

  if (directory.back() != L'\\' && directory.back() != L'/') {
    directory.push_back(L'\\');
  }
  const std::wstring pattern = directory + L"*";

  WIN32_FIND_DATAW found{};
  const FindHandle find(::FindFirstFileExW(
      pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
      FIND_FIRST_EX_LARGE_FETCH));
  VolumeDirectoryListing listing;
  if (find.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    // An empty directory still yields `.` and `..`, so this is a real failure
    // on every file system this product can mount. It is handled rather than
    // assumed away because a mounted volume is removable by nature.
    if (error == ERROR_FILE_NOT_FOUND) return listing;
    throw_last_error(error, "list the directory");
  }

  do {
    if (is_dot_entry(found.cFileName)) continue;
    if (listing.entries.size() >= kVolumeDirectoryEntryLimit) {
      listing.truncated = true;
      break;
    }
    const bool is_directory =
        (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    listing.entries.push_back(VolumeDirectoryEntry{
        .name = found.cFileName,
        .is_directory = is_directory,
        .is_reparse_point =
            (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
        .size_bytes =
            is_directory
                ? 0
                : ((static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) |
                   static_cast<std::uint64_t>(found.nFileSizeLow)),
        .modified_unix_ms = to_unix_milliseconds(found.ftLastWriteTime)});
  } while (::FindNextFileW(find.get(), &found) != FALSE);

  const DWORD error = ::GetLastError();
  if (!listing.truncated && error != ERROR_NO_MORE_FILES && error != 0) {
    throw_last_error(error, "finish listing the directory");
  }

  std::ranges::sort(listing.entries, orders_before);
  return listing;
}

}  // namespace usb_vault::volume
