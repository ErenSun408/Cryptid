#pragma once

#include "usb_vault/application/card_device_lifecycle_controller.h"
#include "usb_vault/application/card_presence_controller.h"

#include <Windows.h>
#include <dbt.h>

#include <cstdint>
#include <string_view>
#include <optional>

namespace usb_vault::platform::windows {

struct HidDeviceIdentity final {
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
};

[[nodiscard]] bool matches_hid_device_path(
    std::wstring_view path, HidDeviceIdentity identity) noexcept;
[[nodiscard]] std::optional<bool> inspect_target_card_event(
    WPARAM event, LPARAM payload, HidDeviceIdentity identity) noexcept;

// Owns one window-scoped HID interface notification registration. Registration
// happens before startup enumeration so a transition cannot be silently lost
// between those operations. Enumeration reads interface metadata only; it does
// not open the device or send HID reports.
class HidCardDeviceNotificationRegistration final {
 public:
  HidCardDeviceNotificationRegistration(HWND window,
                                        HidDeviceIdentity identity);
  ~HidCardDeviceNotificationRegistration() noexcept;

  HidCardDeviceNotificationRegistration(
      const HidCardDeviceNotificationRegistration&) = delete;
  HidCardDeviceNotificationRegistration& operator=(
      const HidCardDeviceNotificationRegistration&) = delete;
  HidCardDeviceNotificationRegistration(
      HidCardDeviceNotificationRegistration&&) = delete;
  HidCardDeviceNotificationRegistration& operator=(
      HidCardDeviceNotificationRegistration&&) = delete;

  [[nodiscard]] bool target_present() const noexcept;

 private:
  HDEVNOTIFY notification_{};
  bool target_present_{};
};

// Safely narrows one WM_DEVICECHANGE payload to the configured HID VID/PID and
// forwards only arrival/removal-complete transitions. Other device classes,
// malformed payloads, and unrelated HID devices are ignored.
[[nodiscard]] application::CardPresenceTransition handle_hid_device_change(
    application::CardPresenceController& controller, WPARAM event,
    LPARAM payload, HidDeviceIdentity identity) noexcept;

// P2 overload: applies the same strict HID payload filter, then coordinates
// entry-context lifetime and existing forced-unmount behavior in their fixed
// security order.
[[nodiscard]] application::CardDeviceLifecycleTransition
handle_hid_device_change(
    application::CardDeviceLifecycleController& controller, WPARAM event,
    LPARAM payload, HidDeviceIdentity identity) noexcept;

}  // namespace usb_vault::platform::windows
