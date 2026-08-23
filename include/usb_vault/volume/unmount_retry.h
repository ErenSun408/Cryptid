#pragma once

#include "usb_vault/volume/volume_provider.h"

namespace usb_vault::volume {

struct UnmountRetryOptions final {
  bool notify_shell{true};
};

// Retries only a normal unmount rejected as busy. The policy never escalates to
// a forced unmount; callers must make any emergency-cleanup choice explicitly.
[[nodiscard]] UnmountResult unmount_with_retry(const VolumeProvider& provider,
                                               wchar_t drive_letter,
                                               const UnmountRetryOptions& options = {});

}  // namespace usb_vault::volume
