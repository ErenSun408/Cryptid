#pragma once

#include <string>
#include <string_view>

namespace usb_vault::application {

struct ExitHotkeySettings final {
  bool enabled{false};
  unsigned int key{0x7B}; // F12
  unsigned int count{3};
  bool operator==(const ExitHotkeySettings&) const = default;
};
[[nodiscard]] bool valid_exit_hotkey(ExitHotkeySettings settings) noexcept;
[[nodiscard]] ExitHotkeySettings exit_hotkey_settings_v1(std::wstring_view path);
void set_exit_hotkey_settings_v1(std::wstring_view path, ExitHotkeySettings settings);

enum class ProductPreference { force_unmount, operation_logging, window_size, auto_mount, close_to_tray, container_extension };
// Container extension values: 0=sys (default), 1=dat, 2=bin, 3=random.
// Neutral preferences only. Defaults: force/auto-mount/close-to-tray off,
// logging on, spacious. Malformed opt-ins read as off. Window presets: 0..3.
[[nodiscard]] unsigned int product_preference_v1(std::wstring_view path, ProductPreference key);
void set_product_preference_v1(std::wstring_view path, ProductPreference key, unsigned int value);

// The preferences this product keeps on disk, and the rules for keeping them.
//
// A settings file is a local artifact, so what may go in it is bounded by 8.4:
// nothing here may record which password opened the session, what a container
// holds, or that a container exists at all. Booleans saying whether the
// product should write paths down, and whether an imported container should be
// mounted at once, say none of those things -- they read the same on a machine
// that has never made a container as on one that has -- which is the same
// ground 8.7.5 gives the auto-mount switch.

// Where the settings live for the signed-in user. Resolving it has no side
// effects.
[[nodiscard]] std::wstring product_settings_v1_default_path();

// Whether the product should remember where containers are.
//
// A file that is not there yet answers yes: that is the product default, and a
// first run has made no choice to honour. A file that is there but cannot be
// read answers no. The two are deliberately different. Absence means nobody
// has chosen; damage means a choice was made and is now unreadable, and the
// only safe reading of an unreadable choice is the one that writes nothing
// down.
[[nodiscard]] bool remember_container_paths_v1(std::wstring_view path);

// Whether importing a container should try to mount it straight away.
//
// Absence and damage answer the same here, because the product default is
// already the quiet one: an import puts the container in the list and stops
// there, and 8.7.5 asks the same of a login. A settings file written before
// this setting existed has no key for it and answers no as well.
[[nodiscard]] bool mount_imported_containers_v1(std::wstring_view path);

// Records one choice, replacing the file in one step and carrying the other
// choice across unchanged. Creates the directory if this is the first thing
// the product has kept.
//
// Carrying the other choice across means carrying what it currently reads as,
// so writing one setting while the file is damaged pins the other at the safe
// answer above. That changes nothing anybody could observe -- the damaged file
// already read that way -- and it leaves a file the user can act on again.
void set_remember_container_paths_v1(std::wstring_view path, bool remember);
void set_mount_imported_containers_v1(std::wstring_view path, bool mount);

}  // namespace usb_vault::application
