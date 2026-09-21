#pragma once

#include <Windows.h>
#include <cstdint>

namespace usb_vault::volume {
struct Fat32SafeSpace final {
  std::uint64_t free_bytes{};
  std::uint32_t cluster_size{};
};

// Read-only scan on a caller-owned, exclusively used volume handle. Both byte
// values must come from authenticated geometry for this same mounted volume.
// Returns a snapshot of whole free clusters before the protected boundary,
// never a guarantee of file payload capacity. Does not open paths or log data.
[[nodiscard]] Fat32SafeSpace scan_fat32_safe_space(
    HANDLE volume, std::uint64_t expected_volume_bytes,
    std::uint64_t protected_start_in_volume);
}  // namespace usb_vault::volume
