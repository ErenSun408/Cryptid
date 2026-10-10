#pragma once

#include <type_traits>

namespace usb_vault::volume::detail {

enum class UnmountNotification { pending, complete, restored };

enum class DrivePresence { absent, present, unknown };

// Retry only namespace refreshes, never delayed drive-removal events. A reused
// letter or an inconclusive probe stops further refreshes after disappearance.
template <typename Probe, typename Complete, typename Refresh, typename Pause>
void refresh_unmounted_drive(Probe&& probe, Complete&& complete,
                             Refresh&& refresh, Pause&& pause) noexcept {
  static_assert(std::is_nothrow_invocable_r_v<DrivePresence, Probe>);
  static_assert(std::is_nothrow_invocable_v<Complete> &&
                std::is_nothrow_invocable_v<Refresh> && std::is_nothrow_invocable_v<Pause>);
  bool completed = false;
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto presence = probe();
    if (presence == DrivePresence::unknown ||
        (presence == DrivePresence::present && completed)) return;
    if (presence == DrivePresence::absent) {
      if (!completed) { complete(); completed = true; }
      refresh();
    }
    if (attempt < 3) pause();
  }
}

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
