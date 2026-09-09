#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace usb_vault::volume {

// Longest single path component, in UTF-16 code units. This is the limit every
// file system a mounted volume can carry imposes on one name.
inline constexpr std::size_t kVolumeEntryNameMaximumLength = 255;

// Longest resolved path, in UTF-16 code units. The file manager reaches the
// file system through plain Win32 calls without the `\\?\` prefix, exactly as
// container creation does, so a path past `MAX_PATH - 1` would fail far from
// here with an error the page could not explain.
inline constexpr std::size_t kVolumePathMaximumLength = 259;

// Turns a drive letter the session vouches for and a relative path the page
// supplied into one absolute path inside that volume.
//
// This is the only reason a page-supplied string may name a file at all. The
// page browses a mounted volume, so it has to be able to say which directory
// it is looking at and which entry it means; everything it can say has to land
// inside the volume the session named, and nothing it can say may reach past
// it. `resolve_container_target_path` states the same kind of guarantee for
// the creation form, and this unit is its counterpart for volume contents.
//
// An empty relative path names the volume root, which is where browsing
// starts. Forward separators are normalized, a single trailing separator is
// dropped, and the result never ends in a separator except at the root.
//
// Rejected, each with `std::invalid_argument`: a drive letter outside `A`-`Z`;
// a relative path that is absolute, rooted, UNC, or in a device namespace; any
// `.` or `..` component; an empty component; a component holding a separator
// class character, a wildcard, a colon, or a control character; a component
// ending in a space or a dot, or starting with a space; a reserved DOS device
// name; and anything past the two limits above.
//
// 🔴 The colon rejection is what keeps an alternate data stream out: `a:b`
// names a stream on `a`, not a file called `a:b`, and a stream is not an entry
// the page can see, delete, or account for.
//
// ⚠️ This check is pure — nothing here touches the file system. It therefore
// establishes only that the *string* cannot leave the volume. A directory
// symlink, junction, or other reparse point inside the volume can still point
// out of it, and no string check can see that. Refusing to follow one is the
// storage layer's obligation, discharged when it opens a handle, not here.
[[nodiscard]] std::wstring resolve_volume_entry_path(
    wchar_t drive_letter, std::wstring_view relative);

// Joins a relative directory the page is looking at with exactly one new
// component it supplied, under the same rules.
//
// Renaming and creating need this instead of the function above, because there
// the page supplies a *name*, not a path. Passing a name through the path
// resolver would let a separator inside it silently move the entry somewhere
// else, so a name is checked as the single component it claims to be and is
// refused if it is anything more.
[[nodiscard]] std::wstring resolve_volume_child_path(
    wchar_t drive_letter, std::wstring_view parent_relative,
    std::wstring_view name);

// Joins a location outside any container with one entry name, for the single
// operation that deliberately writes past a volume: the plaintext export.
//
// 🔴 Every other resolver here exists to keep a path inside a volume. This one
// is the exception, and it is a separate function with a separate name so that
// it cannot be reached by mistake: there is no flag on the others that turns
// them into this, because a flag would eventually be passed wrongly.
//
// `absolute_directory` is a location the user picked, so it is checked the way
// a save location is -- an absolute directory is just a drive root followed by
// a relative path, and every component of it goes through the same rules as a
// component inside a volume. `name` is checked as the single component it
// claims to be, and keeps whatever extension it already had: this copies a
// file out, it does not create a new kind of file.
//
// Rejected with `std::invalid_argument`: anything that is not drive-absolute,
// which covers relative, drive-relative, UNC and device-namespace paths; every
// component fault the resolvers above reject; and a result past the length
// limit. The check is pure, so the caller still has to establish that the
// directory exists.
[[nodiscard]] std::wstring resolve_exported_entry_path(
    std::wstring_view absolute_directory, std::wstring_view name);

}  // namespace usb_vault::volume
