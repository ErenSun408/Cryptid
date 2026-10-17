#include "volume/explorer_drive_name.h"

#include <Windows.h>

#include <string>

namespace usb_vault::volume {
namespace {

constexpr wchar_t kMountPoints[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MountPoints2\\";
constexpr wchar_t kLabelValue[] = L"_LabelFromReg";
// Long enough for any container name the index accepts.
constexpr std::size_t kMaximumNameCharacters = 128;

}  // namespace

std::optional<std::wstring> explorer_drive_name_key(const wchar_t drive_letter) {
  if (drive_letter >= L'A' && drive_letter <= L'Z') return std::wstring(1, drive_letter);
  if (drive_letter >= L'a' && drive_letter <= L'z')
    return std::wstring(1, static_cast<wchar_t>(drive_letter - L'a' + L'A'));
  return std::nullopt;
}

std::wstring publish_explorer_drive_name(const wchar_t drive_letter,
                                         const std::wstring_view name) noexcept {
  try {
    const auto key_name = explorer_drive_name_key(drive_letter);
    if (!key_name) return {};
    const std::wstring path = std::wstring(kMountPoints) + *key_name;
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
      return {};
    }
    LSTATUS status = ERROR_SUCCESS;
    if (name.empty()) {
      status = ::RegDeleteValueW(key, kLabelValue);
      if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    } else {
      std::wstring value(name.substr(0, kMaximumNameCharacters));
      if (!value.empty() && value.back() >= 0xd800 && value.back() <= 0xdbff) value.pop_back();
      status = ::RegSetValueExW(key, kLabelValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS ? *key_name : std::wstring{};
  } catch (...) {
    return {};
  }
}

void withdraw_explorer_drive_name(const std::wstring_view key) noexcept {
  try {
    if (key.size() != 1) return;
    const auto key_name = explorer_drive_name_key(key.front());
    if (!key_name || *key_name != key) return;
    const std::wstring path = std::wstring(kMountPoints) + *key_name;
    HKEY handle = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &handle) != ERROR_SUCCESS) {
      return;
    }
    static_cast<void>(::RegDeleteValueW(handle, kLabelValue));
    ::RegCloseKey(handle);
  } catch (...) {
  }
}

}  // namespace usb_vault::volume
