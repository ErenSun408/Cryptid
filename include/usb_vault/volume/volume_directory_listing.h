#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::volume {

// Most entries one listing carries. It is a safety valve for a pathological
// directory, not a page size: an ordinary directory is orders of magnitude
// below it, and a listing that hits it says so.
inline constexpr std::size_t kVolumeDirectoryEntryLimit = 20000;

// One entry of a directory inside a mounted volume, as the page will see it.
// The name is a single component and never a path, so a listing cannot hand
// the page a string that names somewhere else.
struct VolumeDirectoryEntry final {
  std::wstring name;
  bool is_directory{};
  // A junction, symlink, or other reparse point. It is reported rather than
  // hidden -- a manager that silently omits entries the user can see in
  // Explorer is worse than one that shows them -- but a caller must not
  // descend into one. See the escape check below for why the listing cannot
  // settle that on the caller's behalf.
  bool is_reparse_point{};
  // Zero for a directory. A directory's size is the size of its own record,
  // which is not what anyone reading a file manager expects it to mean.
  std::uint64_t size_bytes{};
  std::int64_t modified_unix_ms{};
};

struct VolumeDirectoryListing final {
  std::vector<VolumeDirectoryEntry> entries;
  // Set when the directory held more than the limit above. The entries kept
  // are then *some* of them, not the first by any ordering the page can see:
  // the limit is applied while enumerating and the ordering afterwards.
  bool truncated{};
};

// Lists one directory inside a mounted volume.
//
// `absolute_directory` must be a drive-absolute path, which is what
// `resolve_volume_entry_path` produces. `.` and `..` are dropped, entries come
// back with directories first and then names in ordinal case-insensitive
// order, and the count is capped.
//
// 🔴 **This is where the volume escape is actually stopped.**
// `resolve_volume_entry_path` can only guarantee that a *string* stays inside
// the volume; a junction or symlink inside the volume points wherever it was
// made to point, and no string check can see that. So this opens the directory
// and asks the file system what it actually opened: if the final path is not
// on the drive the caller named, the directory is refused rather than listed.
// A caller that walks into a directory it found here therefore cannot be
// walked out of the volume, whatever is planted inside it.
//
// Throws `std::invalid_argument` for a path that is not drive-absolute,
// `std::runtime_error` when the path is not a directory or resolves onto
// another volume, and `std::system_error` carrying the Win32 code for
// everything the file system refused.
[[nodiscard]] VolumeDirectoryListing list_volume_directory(
    std::wstring_view absolute_directory);

}  // namespace usb_vault::volume
