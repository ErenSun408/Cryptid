#include "usb_vault/volume/unmount_retry.h"
#include "volume/unmount_retry_policy.h"

namespace usb_vault::volume {

UnmountResult unmount_with_retry(const VolumeProvider& provider,
                                 wchar_t drive_letter,
                                 const UnmountRetryOptions& options) {
  return detail::retry_unmount(provider, drive_letter, options);
}

UnmountEscalationResult unmount_with_forced_escalation(
    const VolumeProvider& provider, wchar_t drive_letter,
    const UnmountRetryOptions& options) {
  return detail::escalate_unmount(provider, drive_letter, options);
}

}  // namespace usb_vault::volume
