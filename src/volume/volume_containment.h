#pragma once

#include <string>
#include <string_view>

namespace usb_vault::volume {

// Shared between listing and the file operations, because both have to settle
// the same question before they touch anything: the caller named a volume, and
// what the file system actually opens has to still be on it.

// Returns the drive letter of a drive-absolute path, or throws
// `std::invalid_argument`. Callers are meant to hand over what
// `resolve_volume_entry_path` produced; checking again costs nothing and makes
// each entry point safe on its own.
[[nodiscard]] wchar_t require_drive_absolute(std::wstring_view path);

// Opens `directory`, following whatever links are in it, and throws
// `std::runtime_error` unless the file system reports it landed on `drive`.
//
// 🔴 This is where a volume escape is actually stopped. A path string can be
// made to read `X:\...` and still arrive on another volume through a junction
// planted inside this one, and no check on the string can see that. Following
// the links is the point: the answer only exists once they are followed.
void require_directory_stays_on_volume(const std::wstring& directory,
                                       wchar_t drive);

// The parent of `path` under the same rule. Every operation that changes an
// entry goes through this first, so an entry reached through a junction that
// leaves the volume is refused before anything is deleted, renamed or moved.
void require_parent_stays_on_volume(const std::wstring& path, wchar_t drive);

// Uppercases one ASCII letter. Drive letters are compared with it, and nothing
// here wants a locale involved in that.
[[nodiscard]] wchar_t upper_ascii(wchar_t character) noexcept;

}  // namespace usb_vault::volume
