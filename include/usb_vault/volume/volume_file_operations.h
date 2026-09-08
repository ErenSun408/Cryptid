#pragma once

#include <string_view>

namespace usb_vault::volume {

// Deletes one entry inside a mounted volume, permanently.
//
// Nothing goes to a recycle bin. A volume's `$Recycle.Bin` would keep taking
// space inside the container while showing the user nothing, and reaching it
// means going through the shell's undo stack, which is one more place a trace
// can settle. Delete here means the entry is gone.
//
// A directory goes with everything inside it.
//
// 🔴 A reparse point is unlinked, never followed. A junction inside the volume
// can name any directory on the machine, so a recursive delete that descended
// into one would empty somewhere the user never named -- their profile, say.
// Directory links are removed with `RemoveDirectoryW`, which takes the link
// and leaves the target alone, and the walk never enters them.
//
// A read-only file is deleted rather than refused: on Windows that attribute
// is a hint, not a permission, and the caller has already decided. The
// attribute is cleared first, on the entry being deleted and on nothing else.
//
// Throws `std::invalid_argument` for a path that is not drive-absolute,
// `std::runtime_error` when the parent resolves onto another volume, and
// `std::system_error` carrying the Win32 code for what the file system
// refused. A failure part-way through a directory leaves the rest in place;
// this reports what stopped it rather than pretending the tree is gone.
void delete_volume_entry(std::wstring_view absolute_path);

// Moves one entry to another place on the same volume. Renaming is the case
// where the two parents are equal, so both go through here: to the file system
// they are one operation, and splitting them in this layer would only mean two
// ways to express the same call.
//
// Both paths are checked, so an entry cannot be moved out through a junction
// any more than it can be deleted through one.
//
// An existing destination is never replaced. Overwriting would destroy a file
// the user did not name, and there is no confirmation at this layer to ask
// with; the caller gets `std::system_error` with `ERROR_ALREADY_EXISTS` and
// decides what to say.
//
// Moving a directory into itself or into its own subtree is refused before the
// file system is asked, because what the file system does with it varies and
// none of the outcomes are ones a user would want.
//
// Both paths must be on the same volume. Crossing volumes is a copy followed
// by a delete, which is a different operation with different failure modes,
// and it does not belong behind a name that promises a rename.
void move_volume_entry(std::wstring_view absolute_source,
                       std::wstring_view absolute_destination);

// Creates one directory. The parent must already exist: this creates the entry
// the user just named and never a chain of them, so a mistyped path fails
// instead of quietly building somewhere new.
void create_volume_directory(std::wstring_view absolute_path);

}  // namespace usb_vault::volume
