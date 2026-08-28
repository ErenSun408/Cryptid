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

// Emergency cleanup for a drive whose key material can no longer be trusted,
// such as a volume left behind by a physically removed card. One normal unmount
// is attempted first so an idle volume is still flushed and closed cleanly; a
// precise busy rejection escalates immediately to a forced unmount rather than
// waiting out the busy window, because no user action can make the removed card
// present again.
// Other driver or transport failures propagate without attempting force.
// A forced unmount discards data that the filesystem has not written yet, so
// this policy is never the default and never runs behind unmount_with_retry.
[[nodiscard]] UnmountEscalationResult unmount_with_forced_escalation(
    const VolumeProvider& provider, wchar_t drive_letter,
    const UnmountRetryOptions& options = {});

}  // namespace usb_vault::volume
