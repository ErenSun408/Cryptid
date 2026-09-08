#include "usb_vault/volume/volume_directory_listing.h"

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

[[nodiscard]] wchar_t upper(const wchar_t character) noexcept {
  return (character >= L'a' && character <= L'z')
             ? static_cast<wchar_t>(character - L'a' + L'A')
             : character;
}

// The caller is meant to hand over what `resolve_volume_entry_path` produced.
// Checking it again costs nothing and means this layer is safe to call even
// from somewhere that forgot to go through the resolver.
[[nodiscard]] wchar_t require_drive_absolute(
    const std::wstring_view directory) {
  if (directory.size() < 3 || directory[1] != L':' ||
      (directory[2] != L'\\' && directory[2] != L'/') ||
      upper(directory[0]) < L'A' || upper(directory[0]) > L'Z') {
    throw std::invalid_argument("directory is not a drive-absolute path");
  }
  return directory[0];
}

[[nodiscard]] std::int64_t to_unix_milliseconds(const FILETIME& time) noexcept {
  const std::int64_t ticks = (static_cast<std::int64_t>(time.dwHighDateTime)
                              << 32) |
                             static_cast<std::int64_t>(time.dwLowDateTime);
  return (ticks - kTicksToUnixEpoch) / kTicksPerMillisecond;
}

class Handle final {
 public:
  explicit Handle(const HANDLE handle) noexcept : handle_(handle) {}
  ~Handle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&&) = delete;
  Handle& operator=(Handle&&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

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

// Opens the directory and asks the file system which drive it really landed
// on. A junction planted inside the volume resolves here and nowhere else: the
// path string still reads `X:\...`, and only the opened handle knows better.
//
// The handle is opened without `FILE_FLAG_OPEN_REPARSE_POINT` on purpose. The
// point is not to refuse to open a link, it is to find out where following it
// arrives, and that answer only exists if it is followed.
void require_directory_stays_on_volume(const std::wstring& directory,
                                       const wchar_t drive) {
  const Handle handle(::CreateFileW(
      directory.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    throw_last_error(::GetLastError(), "open the directory");
  }

  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
    throw_last_error(::GetLastError(), "inspect the directory");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    throw std::runtime_error("the path is not a directory");
  }

  const DWORD length =
      ::GetFinalPathNameByHandleW(handle.get(), nullptr, 0, VOLUME_NAME_DOS);
  if (length == 0) {
    throw_last_error(::GetLastError(), "resolve the directory");
  }
  std::wstring final_path(length, L'\0');
  const DWORD written = ::GetFinalPathNameByHandleW(
      handle.get(), final_path.data(), length, VOLUME_NAME_DOS);
  if (written == 0 || written >= length) {
    throw_last_error(::GetLastError(), "resolve the directory");
  }
  final_path.resize(written);

  // `VOLUME_NAME_DOS` answers as `\\?\X:\...`, and for a drive that is only
  // reachable through a mount point it answers as `\\?\Volume{...}\...`. The
  // second form is not this product's shape -- containers are mounted on drive
  // letters -- and it is refused rather than parsed, because a listing that
  // cannot name its own drive cannot prove it stayed on it.
  constexpr std::wstring_view kPrefix = L"\\\\?\\";
  if (!final_path.starts_with(kPrefix)) {
    throw std::runtime_error("the directory has no drive-letter path");
  }
  const std::wstring_view resolved =
      std::wstring_view(final_path).substr(kPrefix.size());
  if (resolved.size() < 2 || resolved[1] != L':' ||
      upper(resolved[0]) != upper(drive)) {
    throw std::runtime_error("the directory resolves outside its volume");
  }
}

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
