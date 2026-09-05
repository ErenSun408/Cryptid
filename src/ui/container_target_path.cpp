#include "usb_vault/ui/container_target_path.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace usb_vault::ui {
namespace {

// Characters Windows forbids in a file name, plus the separators. A name is a
// single component here, so both separators belong on this list even though a
// path may contain them.
constexpr std::wstring_view kForbiddenNameCharacters = L"<>:\"/\\|?*";

// Characters that must not appear anywhere in a directory, separators aside.
// A colon is handled separately because the drive letter needs exactly one.
constexpr std::wstring_view kForbiddenDirectoryCharacters = L"<>\"|?*";

// Reserved DOS device names. Windows resolves these to devices no matter which
// directory they are written in, so a container named after one would not be a
// file at all.
constexpr std::array<std::wstring_view, 22> kReservedDeviceNames = {
    L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3", L"COM4",
    L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3",
    L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};

[[noreturn]] void reject(const char* const reason) {
  throw std::invalid_argument(reason);
}

[[nodiscard]] bool is_separator(const wchar_t character) noexcept {
  return character == L'\\' || character == L'/';
}

[[nodiscard]] bool is_control(const wchar_t character) noexcept {
  return character < 0x20 || character == 0x7F;
}

[[nodiscard]] wchar_t upper(const wchar_t character) noexcept {
  return (character >= L'a' && character <= L'z')
             ? static_cast<wchar_t>(character - L'a' + L'A')
             : character;
}

[[nodiscard]] bool equals_ignoring_case(const std::wstring_view left,
                                        const std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (upper(left[index]) != upper(right[index])) return false;
  }
  return true;
}

// A component may not end in a space or a dot: Windows silently strips both,
// so the file that appeared would not be the file the user named.
[[nodiscard]] bool has_trailing_space_or_dot(
    const std::wstring_view component) noexcept {
  const wchar_t last = component.back();
  return last == L' ' || last == L'.';
}

[[nodiscard]] bool is_reserved_device_name(
    const std::wstring_view name) noexcept {
  // The reservation covers the stem, so `CON.hc` is a device just as `CON` is.
  const std::size_t dot = name.find(L'.');
  const std::wstring_view stem =
      dot == std::wstring_view::npos ? name : name.substr(0, dot);
  return std::any_of(kReservedDeviceNames.begin(), kReservedDeviceNames.end(),
                     [stem](const std::wstring_view reserved) {
                       return equals_ignoring_case(stem, reserved);
                     });
}

void validate_name(const std::wstring_view name) {
  if (name.empty()) reject("container name is empty");
  if (name.size() > kContainerNameMaximumLength) {
    reject("container name is too long");
  }
  if (name.front() == L' ') reject("container name starts with a space");
  if (has_trailing_space_or_dot(name)) {
    reject("container name ends with a space or a dot");
  }
  for (const wchar_t character : name) {
    if (is_control(character)) {
      reject("container name holds a control character");
    }
    if (kForbiddenNameCharacters.find(character) != std::wstring_view::npos) {
      reject("container name holds a character a file name cannot carry");
    }
  }
  if (is_reserved_device_name(name)) {
    reject("container name is a reserved device name");
  }
}

// Accepts `X:\` and `X:\a\b` only. A UNC share, a `\\?\` or `\\.\` namespace,
// and every relative form are refused: each of them reaches a place the save
// dialog could not have returned under `FOS_FORCEFILESYSTEM`, and widening the
// surface is not what replacing the dialog was for.
void validate_directory(const std::wstring_view directory) {
  if (directory.size() < 3) reject("save location is not an absolute path");
  const wchar_t drive = directory[0];
  if (upper(drive) < L'A' || upper(drive) > L'Z' || directory[1] != L':' ||
      !is_separator(directory[2])) {
    reject("save location is not a drive-absolute path");
  }

  for (std::size_t index = 0; index < directory.size(); ++index) {
    const wchar_t character = directory[index];
    if (is_control(character)) {
      reject("save location holds a control character");
    }
    if (character == L':' && index != 1) {
      reject("save location holds a second colon");
    }
    if (kForbiddenDirectoryCharacters.find(character) !=
        std::wstring_view::npos) {
      reject("save location holds a character a path cannot carry");
    }
  }

  // Walk the components after the root so traversal, empty components, and
  // silently stripped trailing characters are all caught in one pass.
  std::size_t start = 3;
  while (start <= directory.size()) {
    std::size_t end = start;
    while (end < directory.size() && !is_separator(directory[end])) ++end;
    const std::wstring_view component =
        directory.substr(start, end - start);
    if (component.empty()) {
      // Only a single trailing separator may produce an empty component.
      if (end < directory.size()) reject("save location has an empty segment");
    } else {
      if (component == L"." || component == L"..") {
        reject("save location walks through a relative segment");
      }
      if (has_trailing_space_or_dot(component)) {
        reject("save location has a segment ending in a space or a dot");
      }
      if (component.front() == L' ') {
        reject("save location has a segment starting with a space");
      }
      if (is_reserved_device_name(component)) {
        reject("save location holds a reserved device name");
      }
    }
    if (end == directory.size()) break;
    start = end + 1;
  }
}

}  // namespace

std::wstring resolve_container_target_path(const std::wstring_view directory,
                                           const std::wstring_view name) {
  validate_directory(directory);
  validate_name(name);

  std::wstring path(directory);
  std::replace(path.begin(), path.end(), L'/', L'\\');
  if (!is_separator(path.back())) path.push_back(L'\\');
  path.append(name);

  // The name carries the extension only when the user typed it, so append it
  // unless the name already ends in the one this unit owns. Any other
  // extension stays and this one goes after it: the file is always a
  // container, whatever the user called it.
  const std::wstring_view extension = kContainerFileExtension;
  const bool already_extended =
      name.size() > extension.size() &&
      equals_ignoring_case(name.substr(name.size() - extension.size()),
                           extension);
  if (!already_extended) path.append(extension);

  if (path.size() > kContainerPathMaximumLength) {
    reject("container path is too long");
  }
  return path;
}

}  // namespace usb_vault::ui
