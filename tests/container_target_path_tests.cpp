#include "usb_vault/ui/container_target_path.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using usb_vault::ui::kContainerNameMaximumLength;
using usb_vault::ui::kContainerPathMaximumLength;
using usb_vault::ui::resolve_container_target_path;

int g_failures = 0;

void require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

void require_rejects(const std::wstring_view directory,
                     const std::wstring_view name,
                     const char* const message) {
  try {
    static_cast<void>(resolve_container_target_path(directory, name));
  } catch (const std::invalid_argument&) {
    return;
  } catch (...) {
    require(false, message);
    return;
  }
  require(false, message);
}

void require_resolves(const std::wstring_view directory,
                      const std::wstring_view name,
                      const std::wstring_view expected,
                      const char* const message) {
  try {
    require(resolve_container_target_path(directory, name) == expected,
            message);
  } catch (const std::exception&) {
    require(false, message);
  }
}

// The join is the whole point: a name is a file name, the extension belongs to
// this unit, and separators are normalized so a form field may hold either.
void test_a_name_and_a_directory_become_one_path() {
  require_resolves(L"D:\\Vault", L"work", L"D:\\Vault\\work.hc",
                   "a plain name and directory did not join");
  require_resolves(L"D:\\Vault\\", L"work", L"D:\\Vault\\work.hc",
                   "a trailing separator produced a doubled one");
  require_resolves(L"D:/Vault", L"work", L"D:\\Vault\\work.hc",
                   "forward separators were not normalized");
  require_resolves(L"D:\\", L"work", L"D:\\work.hc",
                   "a drive root was not accepted");
  require_resolves(L"d:\\Vault", L"工作资料", L"d:\\Vault\\工作资料.hc",
                   "a non-ASCII name was not carried through");

  // The extension is appended unless the user already typed this one. Any
  // other extension survives and this one goes after it, so a name can never
  // decide how the file is typed.
  require_resolves(L"D:\\Vault", L"work.hc", L"D:\\Vault\\work.hc",
                   "the extension was appended twice");
  require_resolves(L"D:\\Vault", L"work.HC", L"D:\\Vault\\work.HC",
                   "the extension check was case sensitive");
  require_resolves(L"D:\\Vault", L"work.exe", L"D:\\Vault\\work.exe.hc",
                   "another extension was accepted as the container's");
  require_resolves(L"D:\\Vault", L"archive.2019", L"D:\\Vault\\archive.2019.hc",
                   "an interior dot was treated as an extension");
}

void test_a_name_that_is_not_a_file_name_is_refused() {
  require_rejects(L"D:\\Vault", L"", "an empty name was accepted");
  require_rejects(L"D:\\Vault", std::wstring(kContainerNameMaximumLength + 1,
                                             L'a'),
                  "an over-long name was accepted");
  for (const std::wstring_view name :
       {std::wstring_view(L"sub\\work"), std::wstring_view(L"sub/work"),
        std::wstring_view(L"work:stream"), std::wstring_view(L"wo*rk"),
        std::wstring_view(L"wo?rk"), std::wstring_view(L"wo|rk"),
        std::wstring_view(L"wo<rk"), std::wstring_view(L"wo>rk"),
        std::wstring_view(L"wo\"rk")}) {
    require_rejects(L"D:\\Vault", name,
                    "a name holding a forbidden character was accepted");
  }
  require_rejects(L"D:\\Vault", L"wo\trk",
                  "a name holding a control character was accepted");
  require_rejects(L"D:\\Vault", L" work",
                  "a name starting with a space was accepted");
  // Windows strips a trailing space or dot in silence, so the file that
  // appeared would not be the file the user named.
  require_rejects(L"D:\\Vault", L"work ",
                  "a name ending in a space was accepted");
  require_rejects(L"D:\\Vault", L"work.",
                  "a name ending in a dot was accepted");
  require_rejects(L"D:\\Vault", L".", "a name of one dot was accepted");
  require_rejects(L"D:\\Vault", L"..", "a name of two dots was accepted");

  // Reserved device names resolve to devices in every directory, so a
  // container named after one would not be a file at all.
  for (const std::wstring_view name :
       {std::wstring_view(L"NUL"), std::wstring_view(L"nul"),
        std::wstring_view(L"CON"), std::wstring_view(L"aux"),
        std::wstring_view(L"COM1"), std::wstring_view(L"lpt9"),
        std::wstring_view(L"nul.hc"), std::wstring_view(L"CON.backup")}) {
    require_rejects(L"D:\\Vault", name,
                    "a reserved device name was accepted");
  }
  // A name that only starts like one is an ordinary name.
  require_resolves(L"D:\\Vault", L"console", L"D:\\Vault\\console.hc",
                   "a name that merely begins like a device was refused");
  require_resolves(L"D:\\Vault", L"COM0", L"D:\\Vault\\COM0.hc",
                   "COM0 is not a reserved device and must be allowed");
}

// Only a drive-absolute path is accepted. Every other form reaches somewhere
// the save dialog could not have returned, and the form was not meant to widen
// that surface.
void test_only_a_drive_absolute_directory_is_accepted() {
  require_rejects(L"", L"work", "an empty directory was accepted");
  require_rejects(L"Vault", L"work", "a relative directory was accepted");
  require_rejects(L"\\Vault", L"work",
                  "a root-relative directory was accepted");
  require_rejects(L"D:", L"work", "a bare drive letter was accepted");
  require_rejects(L"D:Vault", L"work",
                  "a drive-relative directory was accepted");
  require_rejects(L"1:\\Vault", L"work",
                  "a non-letter drive was accepted");
  require_rejects(L"\\\\server\\share", L"work",
                  "a UNC share was accepted");
  require_rejects(L"\\\\?\\D:\\Vault", L"work",
                  "an extended-length namespace path was accepted");
  require_rejects(L"\\\\.\\PhysicalDrive0", L"work",
                  "a device namespace path was accepted");

  require_rejects(L"D:\\Vault\\..\\Windows", L"work",
                  "a directory walking through .. was accepted");
  require_rejects(L"D:\\Vault\\.\\sub", L"work",
                  "a directory walking through . was accepted");
  require_rejects(L"D:\\Vault\\\\sub", L"work",
                  "a directory with an empty segment was accepted");
  require_rejects(L"D:\\Vault\\sub ", L"work",
                  "a segment ending in a space was accepted");
  require_rejects(L"D:\\Vault\\sub.", L"work",
                  "a segment ending in a dot was accepted");
  require_rejects(L"D:\\Vault\\ sub", L"work",
                  "a segment starting with a space was accepted");
  require_rejects(L"D:\\Vault\\NUL", L"work",
                  "a directory holding a reserved device name was accepted");
  require_rejects(L"D:\\Vault:stream", L"work",
                  "a directory holding a second colon was accepted");
  require_rejects(L"D:\\Va*ult", L"work",
                  "a directory holding a wildcard was accepted");
  require_rejects(L"D:\\Va\tult", L"work",
                  "a directory holding a control character was accepted");

  // Nothing here touches the file system: existence is the caller's check.
  require_resolves(L"D:\\Vault\\deep\\nested", L"work",
                   L"D:\\Vault\\deep\\nested\\work.hc",
                   "a directory that does not exist was rejected here");
}

// Everything downstream reaches the file system without the `\\?\` prefix, so
// a path past `MAX_PATH - 1` would fail far from here.
void test_the_joined_path_stays_inside_the_win32_limit() {
  const std::wstring directory =
      L"D:\\" + std::wstring(kContainerPathMaximumLength - 20, L'a');
  require_resolves(directory, L"work", directory + L"\\work.hc",
                   "a path inside the limit was rejected");
  require_rejects(directory + L"aaaaaaaaaaaaaaaaaaaa", L"work",
                  "a path past the Win32 limit was accepted");
}

[[nodiscard]] bool all_of_set(const std::wstring_view text, const std::wstring_view allowed) {
  return text.find_first_not_of(allowed) == std::wstring_view::npos;
}

void test_a_protected_path_hides_the_name_under_random_ones() {
  using usb_vault::ui::resolve_protected_container_target_path;
  std::vector<std::wstring> folders;
  std::vector<std::wstring> files;
  for (const auto directory : {L"D:\\测试", L"D:\\测试\\", L"D:/测试"}) {
    const std::wstring path = resolve_protected_container_target_path(directory, L"TEST.sys");
    const std::wstring prefix = L"D:\\测试\\";
    require(path.starts_with(prefix), "protected path left the chosen directory");
    const std::wstring_view rest = std::wstring_view(path).substr(prefix.size());
    require(rest.size() == 64 + 1 + 38, "protected path has the wrong shape");
    if (rest.size() != 64 + 1 + 38) continue;
    const std::wstring_view folder = rest.substr(0, 64);
    const std::wstring_view file = rest.substr(65);
    require(rest[64] == L'\\', "container is not one level below the directory");
    require(all_of_set(folder, L"0123456789abcdef"), "folder is not lowercase hexadecimal");
    require(file.front() == L'{' && file.back() == L'}' && file[9] == L'-' && file[14] == L'-' &&
                file[19] == L'-' && file[24] == L'-',
            "file name is not a braced GUID");
    std::wstring digits(file.substr(1, 36));
    std::erase(digits, L'-');
    require(digits.size() == 32 && all_of_set(digits, L"0123456789ABCDEF"), "GUID is not uppercase hexadecimal");
    require(file[15] == L'4' && std::wstring_view(L"89AB").find(file[20]) != std::wstring_view::npos,
            "GUID is not a version 4 RFC 4122 value");
    require(path.find(L"TEST") == std::wstring::npos, "typed name reached the disk path");
    folders.emplace_back(folder);
    files.emplace_back(file);
  }
  for (int i = 0; i < 61; ++i) {
    const std::wstring path = resolve_protected_container_target_path(L"D:\\Vault", L"work");
    folders.push_back(path.substr(9, 64));
    files.push_back(path.substr(74));
  }
  std::ranges::sort(folders);
  std::ranges::sort(files);
  require(std::ranges::adjacent_find(folders) == folders.end(), "random folder name repeated");
  require(std::ranges::adjacent_find(files) == files.end(), "random file name repeated");

  bool rejected = false;
  for (const auto name : {L"", L"a\\b", L"CON.sys", L"work.", L" work", L"a:b"}) {
    rejected = false;
    try { static_cast<void>(resolve_protected_container_target_path(L"D:\\Vault", name)); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "protected path accepted a name the form refuses");
  }
  for (const auto directory : {L"Vault", L"\\\\server\\share", L"D:\\a\\..\\b", L"D:\\a."}) {
    rejected = false;
    try { static_cast<void>(resolve_protected_container_target_path(directory, L"work")); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "protected path accepted a directory the form refuses");
  }
  // 3 + 152 + 1 + 64 + 1 + 38 = 259 fits exactly; one more character does not.
  const std::wstring fits = L"D:\\" + std::wstring(152, L'a');
  require(resolve_protected_container_target_path(fits, L"work").size() == kContainerPathMaximumLength,
          "protected path at the limit was refused");
  rejected = false;
  try { static_cast<void>(resolve_protected_container_target_path(fits + L"a", L"work")); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "protected path past the Win32 limit was accepted");
}

void test_the_display_name_drops_one_known_suffix() {
  using usb_vault::ui::container_display_name;
  require(container_display_name(L"TEST.sys") == L"TEST", ".sys stayed in the display name");
  require(container_display_name(L"TEST.SYS") == L"TEST", "suffix match was case-sensitive");
  for (const auto name : {L"a.hc", L"a.dat", L"a.Bin", L"a.bat"})
    require(container_display_name(name) == L"a", "known suffix stayed in the display name");
  require(container_display_name(L"TEST.sys.dat") == L"TEST.sys", "more than one suffix was removed");
  require(container_display_name(L"report.txt") == L"report.txt", "unknown suffix was removed");
  require(container_display_name(L".sys") == L".sys", "a bare suffix became an empty name");
  require(container_display_name(L"工作") == L"工作", "plain name was altered");
}

}  // namespace

int main() {
  try {
    using usb_vault::ui::choose_container_file_extension;
    for (unsigned int value = 0; value < 3; ++value) {
      const auto extension = choose_container_file_extension(value);
      require(extension == (value == 0 ? L".sys" : value == 1 ? L".dat" : L".bin"), "fixed extension selection failed");
      for (const auto name : {L"work", L"work.HC", L"work.sys", L"work.DAT", L"work.bin"})
        require(resolve_container_target_path(L"D:\\Vault", name, extension) == L"D:\\Vault\\work" + std::wstring(extension),
                "selected extension was doubled or ignored");
    }
    for (int i = 0; i < 64; ++i) {
      const auto extension = choose_container_file_extension(3);
      require(extension == L".sys" || extension == L".dat" || extension == L".bin", "random extension outside allowlist");
    }
    bool rejected = false;
    try { static_cast<void>(choose_container_file_extension(4)); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid preference accepted");
    rejected = false;
    try { static_cast<void>(resolve_container_target_path(L"D:\\Vault", L"work", L".exe")); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "unsafe extension accepted");
    rejected = false;
    try { static_cast<void>(resolve_container_target_path(L"D:\\Vault", L"CON.hc", L".dat")); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "extension override bypassed reserved name protection");
    test_a_name_and_a_directory_become_one_path();
    test_a_name_that_is_not_a_file_name_is_refused();
    test_only_a_drive_absolute_directory_is_accepted();
    test_the_joined_path_stays_inside_the_win32_limit();
    test_a_protected_path_hides_the_name_under_random_ones();
    test_the_display_name_drops_one_known_suffix();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return 1;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "container target path tests passed\n";
  return 0;
}
