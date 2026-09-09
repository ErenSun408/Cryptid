#include "usb_vault/volume/volume_entry_path.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace usb_vault::volume {
namespace {

// Characters a single component may not carry. Both separators are on the list
// because a component is one name: a path may hold separators, a name may not.
// The colon is here rather than handled apart, unlike the container path
// resolver, because no component of a relative path is ever allowed one.
constexpr std::wstring_view kForbiddenComponentCharacters = L"<>:\"/\\|?*";

// Reserved DOS device names. Windows resolves these to devices in every
// directory, so an entry named after one would not be a file at all.
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

[[nodiscard]] bool equals_ignoring_case(
    const std::wstring_view left, const std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (upper(left[index]) != upper(right[index])) return false;
  }
  return true;
}

[[nodiscard]] bool is_reserved_device_name(
    const std::wstring_view name) noexcept {
  // The reservation covers the stem, so `CON.txt` is a device just as `CON` is.
  const std::size_t dot = name.find(L'.');
  const std::wstring_view stem =
      dot == std::wstring_view::npos ? name : name.substr(0, dot);
  return std::any_of(kReservedDeviceNames.begin(), kReservedDeviceNames.end(),
                     [stem](const std::wstring_view reserved) {
                       return equals_ignoring_case(stem, reserved);
                     });
}

// One component of a relative path, or one name the page supplied. Both are
// the same thing, so both are checked here and the caller only decides how
// many of them it is willing to accept.
void validate_component(const std::wstring_view component) {
  if (component.empty()) reject("path has an empty segment");
  if (component.size() > kVolumeEntryNameMaximumLength) {
    reject("name is too long");
  }
  // Traversal is refused by name rather than resolved away. Collapsing `..`
  // here would mean this unit decides what the page meant; refusing means the
  // page can only ever say where it already is.
  if (component == L"." || component == L"..") {
    reject("path walks through a relative segment");
  }
  if (component.front() == L' ') reject("name starts with a space");
  // Windows silently strips a trailing space or dot, so the entry that
  // appeared would not be the entry the page named.
  const wchar_t last = component.back();
  if (last == L' ' || last == L'.') {
    reject("name ends with a space or a dot");
  }
  for (const wchar_t character : component) {
    if (is_control(character)) reject("name holds a control character");
    if (kForbiddenComponentCharacters.find(character) !=
        std::wstring_view::npos) {
      reject("name holds a character a file name cannot carry");
    }
  }
  if (is_reserved_device_name(component)) {
    reject("name is a reserved device name");
  }
}

[[nodiscard]] std::wstring volume_root(const wchar_t drive_letter) {
  if (upper(drive_letter) < L'A' || upper(drive_letter) > L'Z') {
    reject("drive letter is not a drive letter");
  }
  std::wstring root;
  root.push_back(drive_letter);
  root.append(L":\\");
  return root;
}

// Appends every component of `relative` to `path`, checking each one. The
// relative path must be relative: a leading separator would name the volume
// root from the page's side, and letting the page write a rooted path is the
// habit this whole unit exists to prevent.
void append_relative(std::wstring& path, const std::wstring_view relative) {
  if (relative.empty()) return;
  if (is_separator(relative.front())) {
    reject("path is rooted rather than relative");
  }

  // A single trailing separator is ordinary in a directory path, so drop it
  // before walking; anything else empty is a genuine empty segment.
  std::wstring_view remaining = relative;
  if (is_separator(remaining.back())) {
    remaining = remaining.substr(0, remaining.size() - 1);
    if (remaining.empty()) reject("path has an empty segment");
  }

  std::size_t start = 0;
  while (true) {
    std::size_t end = start;
    while (end < remaining.size() && !is_separator(remaining[end])) ++end;
    const std::wstring_view component = remaining.substr(start, end - start);
    validate_component(component);
    path.append(component);
    if (end == remaining.size()) break;
    path.push_back(L'\\');
    start = end + 1;
  }
}

void require_within_limit(const std::wstring& path) {
  if (path.size() > kVolumePathMaximumLength) reject("path is too long");
}

}  // namespace

std::wstring resolve_volume_entry_path(const wchar_t drive_letter,
                                       const std::wstring_view relative) {
  std::wstring path = volume_root(drive_letter);
  append_relative(path, relative);
  require_within_limit(path);
  return path;
}

std::wstring resolve_exported_entry_path(
    const std::wstring_view absolute_directory, const std::wstring_view name) {
  // The name is checked before the location is walked, for the same reason as
  // above: a caller that supplied both a bad location and a bad name is told
  // about the name it just typed.
  validate_component(name);

  // An absolute directory is a drive root and then a relative path, so the
  // three characters of the root are all that needs a rule of its own.
  if (absolute_directory.size() < 3 || absolute_directory[1] != L':' ||
      !is_separator(absolute_directory[2])) {
    reject("export location is not a drive-absolute directory");
  }
  std::wstring path = volume_root(absolute_directory[0]);
  append_relative(path, absolute_directory.substr(3));
  if (!is_separator(path.back())) path.push_back(L'\\');
  path.append(name);
  require_within_limit(path);
  return path;
}

std::wstring resolve_volume_child_path(const wchar_t drive_letter,
                                       const std::wstring_view parent_relative,
                                       const std::wstring_view name) {
  // The name is checked before the parent is walked, so a page that supplies
  // both a bad parent and a bad name is told about the name it just typed
  // rather than about a directory it is already looking at.
  validate_component(name);

  std::wstring path = volume_root(drive_letter);
  append_relative(path, parent_relative);
  if (!is_separator(path.back())) path.push_back(L'\\');
  path.append(name);
  require_within_limit(path);
  return path;
}

}  // namespace usb_vault::volume
