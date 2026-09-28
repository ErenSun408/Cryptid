#pragma once

#include <type_traits>

namespace usb_vault::volume::detail {

enum class UnmountNotification { pending, complete, restored };

// Notification failures must not replace a driver failure or turn a successful
// unmount into a reported failure. The existence probe is best effort, noexcept.
template <typename Unmount, typename Notify, typename Exists>
void unmount_with_notifications(bool enabled, Unmount&& unmount,
                                Notify&& notify, Exists&& exists) {
  static_assert(std::is_nothrow_invocable_v<Notify, UnmountNotification>);
  static_assert(std::is_nothrow_invocable_r_v<bool, Exists>);
  if (enabled) notify(UnmountNotification::pending);
  try {
    unmount();
  } catch (...) {
    if (enabled && exists()) notify(UnmountNotification::restored);
    throw;
  }
  if (enabled) notify(UnmountNotification::complete);
}

}  // namespace usb_vault::volume::detail
