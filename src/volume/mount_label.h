#pragma once

#include <string>
#include <string_view>

namespace usb_vault::volume::detail {

// Use the common FAT/NTFS limit. Do not let the driver's truncation split a
// UTF-16 pair, and never include a path or an inferred volume role.
inline std::wstring mount_display_label(std::wstring_view name) {
  if (name.size() >= 3 && name[name.size() - 3] == L'.' &&
      (name[name.size() - 2] == L'h' || name[name.size() - 2] == L'H') &&
      (name.back() == L'c' || name.back() == L'C')) name.remove_suffix(3);
  std::wstring result;
  constexpr std::wstring_view forbidden = L"\\/\"*?:<>|.,;+=[]";
  for (std::size_t i = 0; i < name.size() && result.size() < 11; ++i) {
    const wchar_t c = name[i];
    if (c >= 0xd800 && c <= 0xdbff) {
      if (i + 1 < name.size() && name[i + 1] >= 0xdc00 && name[i + 1] <= 0xdfff) {
        if (result.size() + 2 > 11) break;
        result.push_back(c);
        result.push_back(name[++i]);
      } else result.push_back(L'_');
    } else if (c < 0x20 || c == 0x7f || (c >= 0xdc00 && c <= 0xdfff) ||
               forbidden.find(c) != std::wstring_view::npos) result.push_back(L'_');
    else result.push_back(c);
  }
  while (!result.empty() && result.back() == L' ') result.pop_back();
  if (result.empty()) result = L"SecureDrive";
  return result;
}

}  // namespace usb_vault::volume::detail
