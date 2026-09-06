#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {

// What a key can see in the index, and how a path enters or leaves it. This
// layer holds the placement policy and nothing else: it does no file I/O, so
// the caller reads the whole file, hands it here, and writes back what comes
// out.

// The key every valid password can reach, derived from the card root secret
// alone rather than from any password.
//
// Every container the product can create today opens under both passwords: a
// standard container takes its volume password from the root, and a container
// with a hidden volume gives its outer volume to one password and its inner
// volume to the other. So every path belongs in every view, and one shared key
// is the honest way to say that.
//
// It also removes a hazard that a per-password key would carry. Entries sealed
// under a password not currently in hand are, by design, indistinguishable
// from filler -- that is what stops a coerced password from counting the
// records it cannot open -- and a writer cannot avoid a slot it cannot see. It
// would therefore overwrite one now and then. Under one shared key every
// occupied slot is visible to the writer, so nothing is ever overwritten.
//
// The day a container arrives that only one password can open (8.7.4 permits
// one, and no entry point offers it yet), its entry has to be sealed under
// that password's own key, and that hazard arrives with it. The format takes
// whatever key it is handed, so only this layer changes.
[[nodiscard]] security::SecureBuffer derive_shared_container_index_key_v1(
    std::span<const std::byte> root_secret);

// Every path in the file this key opens, in slot order. Slot order is not
// creation order and carries no meaning; it is simply where the paths were
// found.
[[nodiscard]] std::vector<std::wstring> list_container_index_paths_v1(
    std::span<const std::byte> key, std::span<const std::byte> file);

enum class ContainerIndexWriteV1 {
  added,
  already_present,
  full,
};

// Seals one path into a free slot, chosen uniformly at random among the slots
// this key cannot open. Position has to stay meaningless, so the choice
// depends neither on the path nor on where anything else sits.
//
// A path already present is left alone rather than written twice; the same
// container reached through the same dialog twice should not appear twice.
// Paths are matched the way Windows matches file names, case insensitively.
// Two spellings that reach the same file by other means -- a short name, a
// junction -- are not recognized as one, and would be remembered twice.
//
// A full file is refused rather than made room in. Evicting an older record
// would mean choosing one to lose, and the only records available to weigh are
// the ones this key can open.
[[nodiscard]] ContainerIndexWriteV1 add_container_index_path_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::wstring_view path);

// Fills the slot holding this path with fresh random bytes, and reports
// whether there was one. Only a slot this key opens can be found, so a record
// of the same container sealed under another key stays where it is; it cannot
// be recognized, and overwriting slots on the chance they hold one would
// destroy unrelated records.
[[nodiscard]] bool remove_container_index_path_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::wstring_view path);

}  // namespace usb_vault::application
