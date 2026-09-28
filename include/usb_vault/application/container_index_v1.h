#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/partition_identity.h"

#include <array>
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

// Optional display alias, stored only inside the authenticated payload.
// Empty means use the original file name or partition sequence. UTF-16 units.
inline constexpr std::size_t kContainerIndexNameMaximumLength = 100;
[[nodiscard]] bool valid_container_index_name(std::wstring_view name) noexcept;

// Seals one path into exactly one slot. Entries of different path lengths
// produce the same size and the same shape: the unused remainder is random,
// not zero, so slot contents never say how long the path is.
//
// With no order this writes the original version 1 path-only payload; with an
// order it writes version 2. A nonempty alias writes version 7, including an
// optional order. All versions have exactly the same slot size.
[[nodiscard]] security::SecureBuffer seal_container_index_entry_v1(
    std::span<const std::byte> key, std::wstring_view path,
    std::optional<std::uint32_t> order = std::nullopt,
    std::wstring_view name = {});

// Stable identity of one supported MBR/GPT 512-byte partition. No drive letter, disk
// number, discovery path, preformat filesystem serial, password or role is
// persisted. A match locates a candidate; only authenticated mounting proves it.
using ContainerIndexPartition = volume::PartitionIdentity;
enum class PartitionConversionState : std::uint32_t { normal = 0, uncertain = 1, paused = 2 };
[[nodiscard]] bool valid_container_index_partition(
    const ContainerIndexPartition& partition) noexcept;

// Version 3 is a partition payload inside the same sealed slot. Never store
// this metadata in a sidecar. Older applications cannot safely edit v3 indexes.
//
// Version 6 supersedes 3, 4 and 5 for everything written from now on: it takes
// the layout from its own field instead of from the version number, always
// carries the conversion state, and adds the creation sequence. A non-zero
// sequence is what makes it a v6 record; passing zero writes the legacy shape
// so existing records can be re-sealed unchanged. A nonempty alias writes v8;
// v8 also permits zero sequence until legacy numbering assigns one. Versions
// 7/8 must not be edited with older clients that cannot recognize them.
[[nodiscard]] security::SecureBuffer seal_container_index_partition(
    std::span<const std::byte> key, const ContainerIndexPartition& partition,
    std::optional<std::uint32_t> order = std::nullopt,
    PartitionConversionState conversion = PartitionConversionState::normal,
    std::uint32_t sequence = 0, std::wstring_view name = {});

// Version 2 adds an authenticated order at offset 524, inside the same sealed
// slot. Version 1 remains readable and has no order. Physical slot placement
// remains random and conveys no display order.
struct ContainerIndexEntry {
  std::wstring path;
  std::optional<std::uint32_t> order;
  // v3: path is empty; v1/v2: partition is empty.
  std::optional<ContainerIndexPartition> partition;
  PartitionConversionState conversion{PartitionConversionState::normal};
  // Which disk container this is, counting the order they were created in.
  // It is what a disk container is called, so it must not move when the user
  // rearranges the list: `order` is the position, this is the identity.
  // Zero means a record written before v6, which recorded no such thing.
  std::uint32_t sequence{};
  // v7 file / v8 partition alias; earlier versions have no alias.
  std::wstring name;
};

// Opens supported file and partition payloads; callers must inspect partition.
[[nodiscard]] std::optional<ContainerIndexEntry> try_open_container_index_entry(
    std::span<const std::byte> key, std::span<const std::byte> slot);

// Returns the path when this key authenticates a supported file slot.
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

// Views of one slot inside a whole-file buffer, so no caller repeats the
// arithmetic. Both reject an index past the end and a buffer that is not a
// whole file.
[[nodiscard]] std::span<const std::byte>
container_index_v1_slot(std::span<const std::byte> file, std::size_t index);

[[nodiscard]] std::span<std::byte>
container_index_v1_mutable_slot(std::span<std::byte> file, std::size_t index);

}  // namespace usb_vault::application
