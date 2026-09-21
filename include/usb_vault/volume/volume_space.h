#pragma once

#include <cstdint>

namespace usb_vault::volume {

// How much of a mounted volume is in use, as the filesystem reports it.
struct VolumeSpace final {
  std::uint64_t total_bytes{};
  std::uint64_t free_bytes{};
};

// Asks the filesystem, which any user may do. Throws std::invalid_argument for
// a letter outside A-Z and std::system_error for what Windows refused.
[[nodiscard]] VolumeSpace volume_space(wchar_t drive_letter);

// Free space at the very end of a volume, from its own allocation bitmap.
struct TailSpace final {
  std::uint64_t bytes{};
  std::uint32_t cluster_size{};
};

// Filesystem tail only: it may include hidden protected space and is not a
// safe-write budget. Reading the bitmap requires an elevated volume handle.
[[nodiscard]] TailSpace scan_volume_tail_space(wchar_t drive_letter);

}  // namespace usb_vault::volume
