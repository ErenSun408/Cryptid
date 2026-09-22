#pragma once
#include "usb_vault/volume/partition_catalog.h"
#include <Windows.h>
#include <winioctl.h>
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
};
[[nodiscard]] PartitionAvailability assess_partition(
    const PartitionObservation& observation) noexcept;
// Read-only prerequisite for an elevated caller that already owns the target
// read-capable synchronous volume handle (GENERIC_READ, typically elevated).
// Does not open/reopen, close, duplicate, seek, lock or dismount it.
// No system/paging/BitLocker authorization or backend support is implied. The
// writer must retain this same handle and apply those separate policy gates.
// Currently requires the discovered NTFS filesystem; RAW is not accepted.
[[nodiscard]] bool matches_partition_handle_identity(
    HANDLE handle, const PartitionCandidate& expected) noexcept;
}  // namespace usb_vault::volume::detail
