#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace usb_vault::volume::detail {

// A FAT volume label holds 11 bytes in the system's OEM code page, so a Chinese
// character takes two of them under code page 936; NTFS allows more. A label
// past those 11 bytes, or with a character the code page cannot hold, is
// refused by the file system and Explorer then shows its default name, so the
// label is cut to fit by whole characters and an unrepresentable one becomes
// '_'. The label never includes a path or an inferred volume role.
inline std::wstring mount_display_label(std::wstring_view name) {
  for (const auto extension : {std::wstring_view(L".hc"), std::wstring_view(L".sys"),
                               std::wstring_view(L".dat"), std::wstring_view(L".bin")}) {
    if (name.size() <= extension.size()) continue;
    bool matches = true;
    for (std::size_t i = 0; i < extension.size(); ++i) {
      auto c = name[name.size() - extension.size() + i];
      if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
      if (c != extension[i]) { matches = false; break; }
    }
    if (matches) { name.remove_suffix(extension.size()); break; }
  }
  constexpr std::size_t kLabelBytes = 11;
  // The OEM bytes one character takes, or 0 when the code page cannot hold it.
  const auto oem_bytes = [](const wchar_t c) -> std::size_t {
    char buffer[8]{};
    BOOL defaulted = FALSE;
    const int written = ::WideCharToMultiByte(CP_OEMCP, WC_NO_BEST_FIT_CHARS, &c, 1, buffer,
                                              static_cast<int>(sizeof(buffer)), nullptr, &defaulted);
    return written <= 0 || defaulted ? 0 : static_cast<std::size_t>(written);
  };
  std::wstring result;
  std::size_t used = 0;
  constexpr std::wstring_view forbidden = L"\\/\"*?:<>|.,;+=[]";
  for (std::size_t i = 0; i < name.size(); ++i) {
    wchar_t c = name[i];
    if (c >= 0xd800 && c <= 0xdbff) {
      // A character outside the BMP is never in an OEM code page; its pair
      // is consumed whole and stands as one '_'.
      if (i + 1 < name.size() && name[i + 1] >= 0xdc00 && name[i + 1] <= 0xdfff) ++i;
      c = L'_';
    } else if (c < 0x20 || c == 0x7f || (c >= 0xdc00 && c <= 0xdfff) ||
               forbidden.find(c) != std::wstring_view::npos) {
      c = L'_';
    }
    std::size_t bytes = oem_bytes(c);
    if (bytes == 0) {
      c = L'_';
      bytes = 1;
    }
    if (used + bytes > kLabelBytes) break;
    result.push_back(c);
    used += bytes;
  }
  while (!result.empty() && result.back() == L' ') result.pop_back();
  if (result.empty()) result = L"SecureDrive";
  return result;
}

}  // namespace usb_vault::volume::detail
