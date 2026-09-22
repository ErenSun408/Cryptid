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
}  // namespace usb_vault::volume::detail
