#pragma once
#include "usb_vault/volume/partition_catalog.h"
#include <Windows.h>
#include <winioctl.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace usb_vault::volume::detail {
struct PartitionObservation final {
  bool windows_volume{};
  bool partition_known{};
  bool extent_known{};
  bool geometry_known{};
  bool filesystem_known{};
  bool device_known{};
  PARTITION_INFORMATION_EX partition{};
  VOLUME_DISK_EXTENTS extents{};  // Only one extent is accepted.
  DISK_GEOMETRY geometry{};
  STORAGE_DEVICE_NUMBER_EX device{};
  std::wstring_view filesystem;
  // USB and SD media may hold exFAT or FAT32 and may lack a hardware ID.
  bool external{};
  // The whole disk's partition table, read only for external media whose
  // driver GUID is random; zero when unread or blank.
  bool table_known{};
  PARTITION_STYLE table_style{PARTITION_STYLE_RAW};
  GUID disk_guid{};
  DWORD mbr_signature{};
};
[[nodiscard]] bool external_bus(STORAGE_BUS_TYPE bus) noexcept;
// The identity a candidate is stored under: the driver GUID, or for external
// media without a hardware ID the partition-table identity. Empty when neither
// is reliable.
struct DeviceIdentity final {
  std::array<std::uint8_t, 16> id{};
  std::uint32_t flags{};
};
[[nodiscard]] std::optional<DeviceIdentity> device_identity(
    const PartitionObservation& observation) noexcept;
[[nodiscard]] PartitionAvailability assess_partition(
    const PartitionObservation& observation) noexcept;
// Structural discovery only; missing/RAW filesystems are expected for ciphertext.
[[nodiscard]] PartitionAvailability assess_partition_location(
    const PartitionObservation& observation) noexcept;
// Retained-handle geometry/identity check for the later mount boundary. Does
// not prove content, system/paging policy, freshness after return or write consent.
[[nodiscard]] bool matches_partition_location_handle(
    HANDLE handle, const PartitionLocation& expected) noexcept;
// Read-only prerequisite for an elevated caller that already owns the target
// read-capable synchronous volume handle (GENERIC_READ, typically elevated).
// Does not open/reopen, close, duplicate, seek, lock or dismount it.
// No system/paging/BitLocker authorization or backend support is implied. The
// writer must retain this same handle and apply those separate policy gates.
// Currently requires the discovered NTFS filesystem; RAW is not accepted.
[[nodiscard]] bool matches_partition_handle_identity(
    HANDLE handle, const PartitionCandidate& expected) noexcept;
}  // namespace usb_vault::volume::detail
