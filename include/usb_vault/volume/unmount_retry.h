#pragma once

#include "usb_vault/volume/volume_provider.h"

namespace usb_vault::volume {

// Retries only a normal unmount rejected as busy. The policy never escalates to
// a forced unmount; callers must make any emergency-cleanup choice explicitly.
[[nodiscard]] UnmountResult unmount_with_retry(const VolumeProvider& provider,
                                               wchar_t drive_letter);

}  // namespace usb_vault::volume
