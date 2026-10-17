#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace usb_vault::volume {

// Explorer's display name for a mounted drive, beyond the 11 OEM bytes a FAT
// label holds. For this product's volumes Explorer reads it per user from
// HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\MountPoints2\<letter>,
// value `_LabelFromReg` -- the key named by the drive letter, not the one
// named by the volume GUID, which only other shell clients consult. Explorer
// reads it whenever it builds a view of the drive, so a new window shows a
// change at once.
//
// 🔴 The name sits in the user's registry while the drive is mounted. A clean
// unmount removes it; a crash, a power loss or a shutdown without unmounting
// leaves it until a mount on the same letter overwrites it.

// The key name for a drive letter, or nothing for a character that is not one.
[[nodiscard]] std::optional<std::wstring> explorer_drive_name_key(wchar_t drive_letter);

// Writes `name` for the letter, or removes a value left there when `name` is
// empty. Returns the key name used, empty on failure. Best effort: never
// throws. Called before the driver mounts, so the drive arrives already named,
// and again once it is up.
[[nodiscard]] std::wstring publish_explorer_drive_name(wchar_t drive_letter,
                                                       std::wstring_view name) noexcept;

// Removes the value written under the key name `publish_explorer_drive_name`
// returned. Best effort.
void withdraw_explorer_drive_name(std::wstring_view key) noexcept;

}  // namespace usb_vault::volume
