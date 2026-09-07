#pragma once

#include <string>
#include <string_view>

namespace usb_vault::application {

// The one preference this product keeps on disk, and the rules for keeping it.
//
// A settings file is a local artifact, so what may go in it is bounded by 8.4:
// nothing here may record which password opened the session, what a container
// holds, or that a container exists at all. A single boolean saying whether
// the product should write paths down says none of those things -- it reads
// the same on a machine that has never made a container as on one that has --
// which is the same ground 8.7.5 gives the auto-mount switch.

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

// Records the choice, replacing the file in one step. Creates the directory
// if this is the first thing the product has kept.
void set_remember_container_paths_v1(std::wstring_view path, bool remember);

}  // namespace usb_vault::application
