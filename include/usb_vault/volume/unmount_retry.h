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

struct UnmountEscalationResult final {
  UnmountResult result;
  bool forced{false};
};

// Explicit forced cleanup after card removal or a confirmed user request.
// One normal unmount is attempted first so an idle volume is still flushed
// and closed cleanly; only a precise busy rejection escalates to force.
// Other driver or transport failures propagate without attempting force.
// A forced unmount discards data that the filesystem has not written yet, so
// this policy is never the default and never runs behind unmount_with_retry.
[[nodiscard]] UnmountEscalationResult unmount_with_forced_escalation(
    const VolumeProvider& provider, wchar_t drive_letter,
    const UnmountRetryOptions& options = {});

}  // namespace usb_vault::volume
