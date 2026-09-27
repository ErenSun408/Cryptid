#include "usb_vault/diagnostics/debug_log.h"
#include "card/vikey/vikey_hid_probe.h"

#include <Windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::card::vikey {
namespace {

constexpr USHORT kVendorId = 0x1987;
constexpr USHORT kProductId = 0x0331;
constexpr USAGE kUsagePage = 0xff00;
constexpr USAGE kUsage = 0x0001;
constexpr wchar_t kDevicePathMarker[] = L"vid_1987&pid_0331";

class Handle final {
 public:
  explicit Handle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
      : value_(value) {}
  ~Handle() noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(value_));
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE value_;
};

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

class PreparsedData final {
 public:
  explicit PreparsedData(PHIDP_PREPARSED_DATA value) noexcept
      : value_(value) {}
  ~PreparsedData() noexcept {
    if (value_ != nullptr) {
      static_cast<void>(::HidD_FreePreparsedData(value_));
    }
  }
  PreparsedData(const PreparsedData&) = delete;
  PreparsedData& operator=(const PreparsedData&) = delete;
  [[nodiscard]] PHIDP_PREPARSED_DATA get() const noexcept { return value_; }

 private:
  PHIDP_PREPARSED_DATA value_{};
};

[[noreturn]] void throw_windows_error(const char* const context) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), context);
}

[[nodiscard]] bool is_target_path(const wchar_t* const path) {
  if (path == nullptr) return false;
  std::wstring lowered(path);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](const wchar_t value) {
                   return static_cast<wchar_t>(std::towlower(value));
                 });
  return lowered.find(kDevicePathMarker) != std::wstring::npos;
}

void require_target_identity(const HANDLE handle) {
  HIDD_ATTRIBUTES attributes{};
  attributes.Size = sizeof(attributes);
  const BOOLEAN read = ::HidD_GetAttributes(handle, &attributes);
  if (read == FALSE ||
      attributes.VendorID != kVendorId ||
      attributes.ProductID != kProductId) {
    USB_VAULT_TRACE("安全卡设备", "HID 身份核对失败",
                    "读取成功 " + usb_vault::diagnostics::flag_text(read != FALSE) + "；Win32 " +
                        std::to_string(read == FALSE ? ::GetLastError() : 0) + "；厂商 " +
                        usb_vault::diagnostics::hex_text(attributes.VendorID) + "；产品 " +
                        usb_vault::diagnostics::hex_text(attributes.ProductID));
    throw std::runtime_error("ViKey HID identity did not match its path");
  }

  PHIDP_PREPARSED_DATA raw_data = nullptr;
  if (::HidD_GetPreparsedData(handle, &raw_data) == FALSE ||
      raw_data == nullptr) {
    throw_windows_error("read ViKey HID preparsed data");
  }
  PreparsedData data(raw_data);
  HIDP_CAPS capabilities{};
  if (::HidP_GetCaps(data.get(), &capabilities) != HIDP_STATUS_SUCCESS ||
      capabilities.UsagePage != kUsagePage ||
      capabilities.Usage != kUsage) {
    throw std::runtime_error("ViKey HID usage did not match the contract");
  }
}

}  // namespace

VikeyHidProbe probe_vikey_hid_access() {
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
      if (::GetLastError() == ERROR_NO_MORE_ITEMS) break;
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
    if (!is_target_path(detail->DevicePath)) continue;

    Handle device(::CreateFileW(
        detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!device) {
      return {.device_present = true,
              .read_write_opened = false,
              .open_error = ::GetLastError()};
    }
    require_target_identity(device.get());
    return {.device_present = true,
            .read_write_opened = true,
            .open_error = ERROR_SUCCESS};
  }
  return {};
}

}  // namespace usb_vault::card::vikey
