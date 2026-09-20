#include "platform/windows/hid_card_device_notifications.h"

#include "card/capture_failure.h"
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <optional>
#include <vector>

namespace usb_vault::platform::windows {
namespace {

constexpr std::size_t kIdentifierTokenLength = 8;
constexpr std::size_t kMaximumInterfacePathCharacters = 32768;

class DeviceInformation final {
 public:
  explicit DeviceInformation(const HDEVINFO value) noexcept : value_(value) {}
  ~DeviceInformation() noexcept {
    if (value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::SetupDiDestroyDeviceInfoList(value_));
    }
  }

  DeviceInformation(const DeviceInformation&) = delete;
  DeviceInformation& operator=(const DeviceInformation&) = delete;

  [[nodiscard]] HDEVINFO get() const noexcept { return value_; }

 private:
  HDEVINFO value_;
};

[[noreturn]] void throw_windows_error(const char*, std::uint32_t code = 1502) {
  usb_vault::card::throw_startup_failure(code, 1, ::GetLastError());
}

[[nodiscard]] int hexadecimal_value(const wchar_t value) noexcept {
  if (value >= L'0' && value <= L'9') return value - L'0';
  if (value >= L'a' && value <= L'f') return value - L'a' + 10;
  if (value >= L'A' && value <= L'F') return value - L'A' + 10;
  return -1;
}

[[nodiscard]] bool token_matches(std::wstring_view token,
                                 std::wstring_view prefix,
                                 const std::uint16_t expected) noexcept {
  if (token.size() != kIdentifierTokenLength || prefix.size() != 4) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    wchar_t actual = token[index];
    if (actual >= L'A' && actual <= L'Z') {
      actual = static_cast<wchar_t>(actual - L'A' + L'a');
    }
    if (actual != prefix[index]) return false;
  }

  std::uint16_t parsed = 0;
  for (std::size_t index = prefix.size(); index < token.size(); ++index) {
    const int digit = hexadecimal_value(token[index]);
    if (digit < 0) return false;
    parsed = static_cast<std::uint16_t>(
        (static_cast<unsigned int>(parsed) << 4U) |
        static_cast<unsigned int>(digit));
  }
  return parsed == expected;
}

[[nodiscard]] bool has_identifier_token(std::wstring_view path,
                                        std::wstring_view prefix,
                                        const std::uint16_t expected) noexcept {
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find_first_of(L"#&", begin);
    const std::wstring_view token =
        path.substr(begin, end == std::wstring_view::npos
                               ? path.size() - begin
                               : end - begin);
    if (token_matches(token, prefix, expected)) return true;
    if (end == std::wstring_view::npos) break;
    begin = end + 1;
  }
  return false;
}

[[nodiscard]] bool present_hid_interface(
    const HidDeviceIdentity identity) {
  GUID hid_class{};
  ::HidD_GetHidGuid(&hid_class);
  DeviceInformation devices(::SetupDiGetClassDevsW(
      &hid_class, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
  if (devices.get() == INVALID_HANDLE_VALUE) {
    throw_windows_error("enumerate present HID interfaces");
  }

  for (DWORD index = 0;; ++index) {
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    if (::SetupDiEnumDeviceInterfaces(devices.get(), nullptr, &hid_class,
                                      index, &interface_data) == FALSE) {
      if (::GetLastError() == ERROR_NO_MORE_ITEMS) return false;
      throw_windows_error("enumerate HID interface");
    }

    DWORD required = 0;
    static_cast<void>(::SetupDiGetDeviceInterfaceDetailW(
        devices.get(), &interface_data, nullptr, 0, &required, nullptr));
    if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_windows_error("size HID interface path");
    }

    std::vector<std::byte> detail_storage(required);
    auto* const detail =
        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detail_storage.data());
    detail->cbSize = sizeof(*detail);
    if (::SetupDiGetDeviceInterfaceDetailW(
            devices.get(), &interface_data, detail, required, nullptr,
            nullptr) == FALSE) {
      throw_windows_error("read HID interface path");
    }

    constexpr std::size_t name_offset =
        offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath);
    if (required <= name_offset) {
      usb_vault::card::throw_startup_failure(1503);
    }
    const std::size_t maximum_characters =
        (static_cast<std::size_t>(required) - name_offset) / sizeof(wchar_t);
    const wchar_t* const terminator = static_cast<const wchar_t*>(
        std::wmemchr(detail->DevicePath, L'\0', maximum_characters));
    if (terminator == nullptr) {
      usb_vault::card::throw_startup_failure(1503);
    }
    if (matches_hid_device_path(
            std::wstring_view(
                detail->DevicePath,
                static_cast<std::size_t>(terminator - detail->DevicePath)),
            identity)) {
      return true;
    }
  }
}

[[nodiscard]] std::optional<bool> target_card_arrival(
    const WPARAM event, const LPARAM payload,
    const HidDeviceIdentity identity) noexcept {
  if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE) {
    return std::nullopt;
  }
  if (payload == 0) return std::nullopt;

  const auto* const header =
      reinterpret_cast<const DEV_BROADCAST_HDR*>(payload);
  constexpr std::size_t name_offset =
      offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name);
  if (header->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE ||
      header->dbch_size < name_offset + sizeof(wchar_t) ||
      header->dbch_size >
          name_offset +
              kMaximumInterfacePathCharacters * sizeof(wchar_t)) {
    return std::nullopt;
  }

  const auto* const interface_data =
      reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(payload);
  GUID hid_class{};
  ::HidD_GetHidGuid(&hid_class);
  if (::IsEqualGUID(interface_data->dbcc_classguid, hid_class) == FALSE) {
    return std::nullopt;
  }

  const std::size_t path_bytes = header->dbch_size - name_offset;
  if (path_bytes % sizeof(wchar_t) != 0) return std::nullopt;
  const std::size_t maximum_characters = path_bytes / sizeof(wchar_t);
  const wchar_t* const terminator = static_cast<const wchar_t*>(std::wmemchr(
      interface_data->dbcc_name, L'\0', maximum_characters));
  if (terminator == nullptr ||
      !matches_hid_device_path(
          std::wstring_view(
              interface_data->dbcc_name,
              static_cast<std::size_t>(terminator -
                                       interface_data->dbcc_name)),
          identity)) {
    return std::nullopt;
  }
  return event == DBT_DEVICEARRIVAL;
}

}  // namespace

std::optional<bool> inspect_target_card_event(
    WPARAM event, LPARAM payload, HidDeviceIdentity identity) noexcept {
  return target_card_arrival(event, payload, identity);
}

bool matches_hid_device_path(const std::wstring_view path,
                             const HidDeviceIdentity identity) noexcept {
  return has_identifier_token(path, L"vid_", identity.vendor_id) &&
         has_identifier_token(path, L"pid_", identity.product_id);
}

HidCardDeviceNotificationRegistration::
    HidCardDeviceNotificationRegistration(const HWND window,
                                          const HidDeviceIdentity identity) {
  if (window == nullptr) {
    throw std::invalid_argument("device notification window is required");
  }

  DEV_BROADCAST_DEVICEINTERFACE_W filter{};
  filter.dbcc_size = sizeof(filter);
  filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  ::HidD_GetHidGuid(&filter.dbcc_classguid);
  notification_ = static_cast<HDEVNOTIFY>(::RegisterDeviceNotificationW(
      window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE));
  if (notification_ == nullptr) {
    throw_windows_error("register HID device notifications", 1501);
  }

  try {
    target_present_ = present_hid_interface(identity);
  } catch (...) {
    static_cast<void>(::UnregisterDeviceNotification(notification_));
    notification_ = nullptr;
    throw;
  }
}

HidCardDeviceNotificationRegistration::
    ~HidCardDeviceNotificationRegistration() noexcept {
  if (notification_ != nullptr) {
    static_cast<void>(::UnregisterDeviceNotification(notification_));
  }
}

bool HidCardDeviceNotificationRegistration::target_present() const noexcept {
  return target_present_;
}

application::CardPresenceTransition handle_hid_device_change(
    application::CardPresenceController& controller, const WPARAM event,
    const LPARAM payload, const HidDeviceIdentity identity) noexcept {
  const std::optional<bool> arrival =
      target_card_arrival(event, payload, identity);
  if (!arrival.has_value()) {
    return application::CardPresenceTransition::no_change;
  }

  return *arrival ? controller.handle_arrival()
                  : controller.handle_removal();
}

application::CardDeviceLifecycleTransition handle_hid_device_change(
    application::CardDeviceLifecycleController& controller,
    const WPARAM event, const LPARAM payload,
    const HidDeviceIdentity identity) noexcept {
  const std::optional<bool> arrival =
      target_card_arrival(event, payload, identity);
  if (!arrival.has_value()) {
    return application::CardDeviceLifecycleTransition{
        .presence = application::CardPresenceTransition::no_change,
        .entry_route = controller.current_route(),
        .entry_changed = false,
    };
  }
  return *arrival ? controller.handle_arrival()
                  : controller.handle_removal();
}

}  // namespace usb_vault::platform::windows
