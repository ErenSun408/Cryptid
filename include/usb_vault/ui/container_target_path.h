#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace usb_vault::ui {

// The extension every container file carries. The page never states it: the
// user names a container, and the extension is appended here so a name the
// page supplies cannot decide how the file is typed.
inline constexpr wchar_t kContainerFileExtension[] = L".hc";

// Longest container name the form accepts, in UTF-16 code units. It leaves
// room for a directory of ordinary depth inside the 259-character limit below.
inline constexpr std::size_t kContainerNameMaximumLength = 96;

// Longest resolved path, in UTF-16 code units. Everything downstream reaches
// the file system through plain Win32 calls without the `\\?\` prefix, so a
// path that exceeds `MAX_PATH - 1` would fail far from here with an error the
// page could not explain.
inline constexpr std::size_t kContainerPathMaximumLength = 259;

// Joins a directory and a container name the page supplied into one absolute
// path, and rejects everything the page must not be able to express.
//
// This is the whole reason a page-supplied string may reach the file system at
// all. The form replaced a native save dialog that owned the entire selection,
// so the guarantee the dialog gave structurally now has to be stated: the
// result is always a drive-absolute path to a regular file directly inside the
// named directory, carrying this unit's extension and no other.
//
// Rejected, each with `std::invalid_argument`: a relative, UNC, or device
// path; any `.` or `..` component; a name holding a separator, a wildcard, a
// colon, a control character, or a reserved DOS device name; a name or
// component that ends in a space or a dot; and anything longer than the limits
// above. The check is pure — nothing here touches the file system, so the
// caller still has to establish that the directory exists.
[[nodiscard]] std::wstring resolve_container_target_path(
    std::wstring_view directory, std::wstring_view name);

}  // namespace usb_vault::ui
