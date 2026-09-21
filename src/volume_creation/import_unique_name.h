#pragma once
#include <Windows.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace usb_vault::volume_creation::detail {
// Retry only atomic name collisions, never overwrite or probe-then-open.
template<class Create>
auto create_unique_import(std::wstring_view original, bool directory, Create create) {
  const auto dot = original.find_last_of(L'.');
  const auto split = !directory && dot != std::wstring_view::npos && dot != 0
      ? dot : original.size();
  const std::wstring extension(original.substr(split));
  for (unsigned attempt = 0; attempt < 10000; ++attempt) {
    std::wstring name(original);
    if (attempt) {
      const auto suffix = L" (" + std::to_wstring(attempt) + L")";
      if (extension.size() + suffix.size() >= 255)
        throw std::invalid_argument("import extension leaves no room for suffix");
      auto length = (std::min)(split, 255 - extension.size() - suffix.size());
      // Never split a UTF-16 surrogate pair when shortening a long stem.
      if (length && original[length-1] >= 0xd800 && original[length-1] <= 0xdbff) --length;
      if (!length) throw std::invalid_argument("import filename stem is empty");
      name = std::wstring(original.substr(0, length)) + suffix + extension;
    }
    try { return create(name); }
    catch (const std::system_error& error) {
      if (error.code().category() != std::system_category() ||
          (error.code().value() != ERROR_FILE_EXISTS &&
           error.code().value() != ERROR_ALREADY_EXISTS)) throw;
    }
  }
  throw std::system_error(ERROR_FILE_EXISTS, std::system_category(), "import name candidates exhausted");
}
}
