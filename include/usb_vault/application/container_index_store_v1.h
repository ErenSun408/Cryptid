#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {

// The file that holds the container path index, and nothing about what is in
// it. This layer moves whole files and whole slots; it never holds a key and
// never looks inside a slot, so it cannot tell an entry from filler and has no
// count of either to leak.
//
// The buffer is a plain vector rather than a SecureBuffer on purpose. Every
// byte of the file is already sealed, so there is nothing here worth locking
// into memory, and locking 192 KB would spend the process working-set budget
// that the short-lived plaintext buffers actually need.

// Where the index lives for the signed-in user. Resolving it has no side
// effects; it is `ensure_container_index_v1_file` that creates anything.
[[nodiscard]] std::wstring container_index_v1_default_path();

// Creates the file at its full size with every slot random, and returns
// whether this call is what created it.
//
// It is made whole on first run rather than grown as entries arrive, because a
// file that appeared the day the first container was made, or that grew when
// the second one was, would report through its own existence and size exactly
// what its contents are sealed to withhold.
//
// A file already there and already the right size is left untouched.
bool ensure_container_index_v1_file(std::wstring_view path);

// Reads the whole file. A file of any other size is damage, and this refuses
// it rather than starting over from a fresh random one: the records it would
// discard include ones the password in hand cannot open, so nothing at this
// layer could weigh what was being thrown away.
[[nodiscard]] std::vector<std::byte>
read_container_index_v1_file(std::wstring_view path);

// Replaces the whole file in one step, so no reader ever sees a torn slot and
// a failure part-way leaves the previous file exactly as it was.
//
// Callers rewrite the file by reading it, changing the one slot they mean to
// change, and passing the result back. Every other slot has to arrive back
// byte for byte, filler included. Re-randomizing the slots that were not
// touched looks harmless -- they are random anyway -- but it would mean that
// anyone comparing two copies of the file over time could see which slots did
// not change, and those are precisely the occupied ones. That is the whole
// count, positions included, of records their own password cannot open.
void write_container_index_v1_file(std::wstring_view path,
                                   std::span<const std::byte> contents);

}  // namespace usb_vault::application
