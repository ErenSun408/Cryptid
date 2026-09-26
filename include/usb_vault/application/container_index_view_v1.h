#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/application/container_index_v1.h"

#include <cstddef>
#include <optional>
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

// Every path this key opens, in authenticated display order. Legacy entries
// without an order follow ordered entries in their original slot order.
[[nodiscard]] std::vector<std::wstring> list_container_index_paths_v1(
    std::span<const std::byte> key, std::span<const std::byte> file);

enum class ContainerIndexWriteV1 {
  added,
  already_present,
  full,
};

// Partitions share the same root-derived key and fixed-size file. File-only
// views never expose them as paths. Ordering uses both kinds together.
[[nodiscard]] std::vector<ContainerIndexEntry> list_container_index_partition_entries(
    std::span<const std::byte> key, std::span<const std::byte> file);
// Exact single-record update. No mutation if missing, duplicate or crypto failure.
[[nodiscard]] bool set_container_index_partition_state(std::span<const std::byte> key,
    std::span<std::byte> file, const ContainerIndexPartition& partition, PartitionConversionState state,
    std::uint32_t* assigned_sequence = nullptr);
[[nodiscard]] std::vector<ContainerIndexPartition> list_container_index_partitions(
    std::span<const std::byte> key, std::span<const std::byte> file);
[[nodiscard]] ContainerIndexWriteV1 add_container_index_partition(
    std::span<const std::byte> key, std::span<std::byte> file,
    const ContainerIndexPartition& partition,
    PartitionConversionState state = PartitionConversionState::normal,
    std::uint32_t* assigned_sequence = nullptr);
[[nodiscard]] bool remove_container_index_partition(
    std::span<const std::byte> key, std::span<std::byte> file,
    const ContainerIndexPartition& partition);

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

// Ordered insertion for the UI integration: a genuinely new path goes first;
// adding an existing path leaves its position and the entire buffer unchanged.
[[nodiscard]] ContainerIndexWriteV1 prepend_container_index_path_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::wstring_view path);

// One container an ordering names: a file container by its path, or a disk
// container by its identity. The user arranges both kinds in a single list, so
// both have to be nameable by the same call. Exactly one of the two is set.
struct ContainerIndexRef final {
  std::wstring path;
  std::optional<ContainerIndexPartition> partition;
};

// Shared identity rule for index ordering and session restoration. A partition
// is matched by its complete identity, ignoring its session display path;
// files use case-insensitive ordinal paths. Different kinds never match.
// Invalid references match nothing. No filesystem lookup or path aliasing.
[[nodiscard]] bool container_index_ref_matches_v1(
    const ContainerIndexRef& reference, std::wstring_view path,
    const std::optional<ContainerIndexPartition>& partition);

// Places the requested known containers first, preserving the relative order
// of omitted ones (including containers on disconnected disks). Rejects
// unknown or repeated entries before mutation. Re-seals only entries whose
// order changes; unchanged records, opaque slots and file size are preserved. An exception
// leaves the supplied buffer unchanged. The caller still owns atomic file
// replacement.
//
// Both kinds share one order, because the list the user drags holds both. A
// disk container used to be excluded here, which left it with no order at all:
// it then sorted by the slot it happened to land in, and that slot is chosen at
// random on purpose, so its position was arbitrary and moved whenever another
// record was written.
void reorder_container_index_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::span<const ContainerIndexRef> order);

// Path-based adapter to the same ordering: requested files go first, followed
// by all omitted files and partitions in their previous relative order.
// Partition ranks may change; their identities, states and sequences do not.
void reorder_container_index_paths_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::span<const std::wstring> paths);

// Every container this key opens, in authenticated display order, naming each
// one the way an ordering does. The caller can rank its own rows against this
// without caring which kind each is.
[[nodiscard]] std::vector<ContainerIndexRef> list_container_index_refs_v1(
    std::span<const std::byte> key, std::span<const std::byte> file);

// One operation-local decoded snapshot, never retained across file changes.
[[nodiscard]] std::vector<ContainerIndexEntry> list_container_index_entries_v1(
    std::span<const std::byte> key, std::span<const std::byte> file);

struct PreparedContainerIndexV1 final {
  std::vector<ContainerIndexEntry> entries;
  bool numbered{};
};
// Scans once, assigns missing partition numbers using the smallest unused
// positive number, and returns the updated ordered entries. Existing numbers
// and unrelated slots remain unchanged; exceptions leave the buffer unchanged.
[[nodiscard]] PreparedContainerIndexV1 prepare_container_index_v1(
    std::span<const std::byte> key, std::span<std::byte> file);

// Insertion/state-update sequence outputs are assigned only on added,
// already_present, or successful state update; full/missing/errors leave them
// unchanged. New partitions reuse the smallest unused positive number.

// Gives every partition record that has no creation sequence one, and reports
// whether any did. Records written before v6 carry no such number, and what a
// disk container is called depends on it, so it is settled once rather than
// recomputed per session. Nothing recorded creation order before, so for those
// records the numbers follow the positions they currently hold.
[[nodiscard]] bool number_container_index_partitions_v1(
    std::span<const std::byte> key, std::span<std::byte> file);

// Ordered insertion, the disk-container counterpart of the path prepend: a
// genuinely new partition goes first; one already recorded keeps its position
// and leaves the buffer unchanged.
[[nodiscard]] ContainerIndexWriteV1 prepend_container_index_partition(
    std::span<const std::byte> key, std::span<std::byte> file,
    const ContainerIndexPartition& partition,
    PartitionConversionState state = PartitionConversionState::normal,
    std::uint32_t* assigned_sequence = nullptr);

// Fills the slot holding this path with fresh random bytes, and reports
// whether there was one. Only a slot this key opens can be found, so a record
// of the same container sealed under another key stays where it is; it cannot
// be recognized, and overwriting slots on the chance they hold one would
// destroy unrelated records.
[[nodiscard]] bool remove_container_index_path_v1(
    std::span<const std::byte> key, std::span<std::byte> file,
    std::wstring_view path);

}  // namespace usb_vault::application
