#pragma once

#include <functional>
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
// and it does not belong behind a name that promises a rename. That one is
// `move_volume_entry_by_copying` below.
void move_volume_entry(std::wstring_view absolute_source,
                       std::wstring_view absolute_destination);

// Asked repeatedly while a copy runs; return true to stop it. It is asked
// before each entry and while a large file is in flight, so a cancel takes
// effect within moments rather than at the end of whatever is being written.
//
// An empty function means the copy cannot be cancelled. It is called on the
// thread that called the copy, must not touch the volume itself, and must not
// throw: an exception cannot be let back into the file system's own frames, so
// one thrown here stops the copy as though it had answered true.
using TransferCancellation = std::function<bool()>;

// Copies one entry to another place, on this volume or on another one. A
// directory goes with everything inside it.
//
// **The destination is never replaced and never merged into.** A copy onto an
// existing name fails with `ERROR_FILE_EXISTS` or `ERROR_ALREADY_EXISTS`, and
// what was already there is left exactly as it was, down to not being cleaned
// up when the copy is abandoned: that entry belongs to the user, not to this
// operation.
//
// **Nothing half-written is left behind.** Whatever this created -- the one
// file, or the whole destination tree -- is removed again if the copy fails or
// is cancelled part-way. The removal is best-effort: if it too fails, the
// failure reported is still the one that stopped the copy, because that is the
// one that explains what happened.
//
// **A reparse point is refused rather than copied**, wherever it sits in the
// source. Following it would pull in whatever it names, which is off the
// volume and possibly enormous; recreating it at the destination would plant a
// way out of the container that the rest of this layer then has to keep
// refusing. Neither is a copy, so the operation says so instead. The whole
// source is walked for links before a single byte is written, so this refusal
// costs nothing that has to be undone.
//
// **The room is checked before the copy starts**, and a copy that cannot fit
// fails with `ERROR_DISK_FULL` having written nothing. This is a courtesy,
// not a guarantee: free space can change between the check and the write, so
// the file system can still run out part-way, and that failure is handled the
// same way as any other -- the partial destination goes.
//
// File timestamps and attributes come across, as `CopyFileEx` copies them. A
// directory's own timestamps do not; the copy is a new directory holding the
// same entries.
//
// Throws `std::invalid_argument` for a path that is not drive-absolute or for
// a directory copied into itself, `std::runtime_error` when either parent
// resolves onto another volume or the source holds a link, and
// `std::system_error` carrying the Win32 code for everything else, including
// `ERROR_CANCELLED` when the cancellation asked it to stop.
void copy_volume_entry(std::wstring_view absolute_source,
                       std::wstring_view absolute_destination,
                       const TransferCancellation& cancelled = {});

// Copies one entry to another place and then deletes the source. This is the
// move that works across volumes, where a rename cannot: to the file system
// two containers are two volumes, and there is no operation that moves an
// entry between them.
//
// It is a separate name from `move_volume_entry` because it is a separate
// operation, not an implementation detail of one. It reads and writes every
// byte, it takes as long as the data is large, it can be cancelled, and it can
// run out of room -- none of which a rename can do. A caller that already
// knows both paths are on one volume should use `move_volume_entry`, which is
// a single instant call; this one accepts that case too and will simply do the
// slow thing.
//
// The source is deleted only after the copy has fully succeeded. A cancelled
// or failed copy leaves the source untouched and takes the partial destination
// away with it, so nothing is lost. If the copy succeeds and the delete then
// fails, the entry exists in both places and the delete's failure is what is
// reported -- the error leaves a duplicate rather than a hole, which is the
// direction to fail in.
void move_volume_entry_by_copying(std::wstring_view absolute_source,
                                  std::wstring_view absolute_destination,
                                  const TransferCancellation& cancelled = {});

// Creates one directory. The parent must already exist: this creates the entry
// the user just named and never a chain of them, so a mistyped path fails
// instead of quietly building somewhere new.
void create_volume_directory(std::wstring_view absolute_path);

}  // namespace usb_vault::volume
