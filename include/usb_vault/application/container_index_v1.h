#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace usb_vault::application {

// The on-disk shape of the container path index.
//
// The file is one fixed-size array of fixed-size slots, created full and never
// resized. Nothing about it is plaintext: there is no magic value, no version
// field, and no slot count outside the sealed slots, because a reader who can
// recognize the file at all can also count how many of its slots are in use.
// The whole file has to stay indistinguishable from the random bytes it is
// first written with, so the format version lives inside each sealed slot.
inline constexpr std::size_t kContainerIndexV1SlotSize = 768;
inline constexpr std::size_t kContainerIndexV1SlotCount = 1024;
inline constexpr std::size_t kContainerIndexV1FileSize =
    kContainerIndexV1SlotSize * kContainerIndexV1SlotCount;

// Keys arrive already derived; this unit never sees a password or the root
// secret. Which key seals a slot is the caller's statement about what can open
// the container behind it, and the caller knows that by construction: it holds
// the passwords it just formatted the volumes with. Nothing here infers it.
inline constexpr std::size_t kContainerIndexV1KeySize = 32;

inline constexpr std::uint32_t kContainerIndexEntryV1Version = 1;

// Longest path a slot can carry, in UTF-16 code units. It matches the limit
// the creation form already enforces, so a path that reaches the file system
// also fits here.
inline constexpr std::size_t kContainerIndexV1PathMaximumLength = 259;

// Seals one path into exactly one slot. Entries of different path lengths
// produce the same size and the same shape: the unused remainder is random,
// not zero, so slot contents never say how long the path is.
//
// A v1 slot carries only the path. The version lives inside the sealed bytes
// and the sealed area is deliberately larger than v1 needs, so a later version
// can carry more without changing the size of the file.
[[nodiscard]] security::SecureBuffer seal_container_index_entry_v1(
    std::span<const std::byte> key, std::wstring_view path);

// Returns the path when this key authenticates this slot.
//
// Random filler, a slot sealed under a different key, and a damaged slot are
// one answer: nothing. The caller cannot tell them apart, and neither can
// anyone holding the file who has one of the keys but not the others. That is
// the whole reason a decoy password can hold the file and still not learn that
// there are entries in it that it cannot open.
[[nodiscard]] std::optional<std::wstring> try_open_container_index_entry_v1(
    std::span<const std::byte> key, std::span<const std::byte> slot);

// Fills a slot with bytes that a holder of any key cannot distinguish from a
// sealed entry. Every slot of a new file starts this way.
void fill_container_index_slot_v1(std::span<std::byte> slot);

}  // namespace usb_vault::application
