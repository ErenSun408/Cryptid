#pragma once

#include "usb_vault/volume/partition_identity.h"
#include <optional>
#include <array>
#include <span>
#include <cstdint>
#include <string>
#include <vector>

namespace usb_vault::volume {
// A candidate is only a read-only discovery result, never write authorization.
enum class PartitionAvailability {
  candidate, windows_volume, metadata_unavailable, unsupported_layout,
  protected_partition, inconsistent_extent, unsupported_sector_size,
  unsupported_filesystem, device_identity_unavailable
};
enum class PartitionLayout { unknown, mbr, gpt };
struct PartitionCandidate final {
  std::wstring volume_path;  // Native-only volume GUID path; never a UI input.
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint32_t logical_sector_bytes{};
  PartitionAvailability availability{PartitionAvailability::metadata_unavailable};
  std::uint32_t query_error{};
  PartitionLayout layout{PartitionLayout::unknown};
  std::array<std::uint8_t, 16> partition_id{}; // GPT only; not a hardware identity.
  std::uint32_t filesystem_serial{}; // Detects ordinary reformatting, not cloning.
  std::array<std::uint8_t, 16> device_id{}; // Driver-derived hardware GUID, native only.
  std::uint32_t device_id_flags{};
  // Descriptive media facts, read again wherever a candidate is enumerated, so
  // they take no part in identity: a USB or SD bus, and an NTFS filesystem
  // (only NTFS can be encrypted in place).
  bool external{};
  bool ntfs{};
  bool operator==(const PartitionCandidate& other) const noexcept {
    return volume_path == other.volume_path && disk_number == other.disk_number &&
        partition_number == other.partition_number && offset_bytes == other.offset_bytes &&
        length_bytes == other.length_bytes && logical_sector_bytes == other.logical_sector_bytes &&
        availability == other.availability && query_error == other.query_error &&
        layout == other.layout && partition_id == other.partition_id &&
        filesystem_serial == other.filesystem_serial && device_id == other.device_id &&
        device_id_flags == other.device_id_flags;
  }
};
// Enumerates volume-backed partitions only, including those without drive letters.
// Missing/protected partitions may not have Windows volume objects. No writes,
// mounting, privilege escalation, password handling or persisted inventory.
// Throws if Windows-volume identification or the enumeration itself fails.
[[nodiscard]] std::vector<PartitionCandidate> enumerate_partition_candidates();
// Display-only drive letters and volume label. Never an identity or write permit.
[[nodiscard]] std::wstring partition_display_name(const PartitionCandidate& candidate);
// Requires exactly one unchanged, eligible native snapshot. This is selection
// freshness only, never a retained handle or a write permit. Device identity
// comes from the storage driver and is not proof against forged hardware IDs.
[[nodiscard]] bool matches_partition_snapshot(const PartitionCandidate& selected,
    std::span<const PartitionCandidate> current) noexcept;
// Separate from format candidates: discovery does not inspect a filesystem and
// is never an input to the destructive formatting API. Only current safe MBR
// metadata is returned. A saved identity may locate an unencrypted/replaced
// volume too; a later authenticated mount must verify contents independently.
struct PartitionLocation final {
  PartitionIdentity identity;
  std::wstring volume_path; // Current native GUID path, never persisted/UI input.
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  bool operator==(const PartitionLocation&) const = default;
};
[[nodiscard]] std::vector<PartitionLocation> enumerate_partition_locations();
// Pure lookup: accepts exactly one identity and one unambiguous current path.
// Missing, unsupported, malformed or duplicate inventory entries fail closed.
[[nodiscard]] std::optional<PartitionLocation> resolve_partition_location(
    const PartitionIdentity& saved, std::span<const PartitionLocation> current);
}  // namespace usb_vault::volume
