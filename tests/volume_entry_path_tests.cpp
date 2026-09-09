#include "usb_vault/volume/volume_entry_path.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using usb_vault::volume::kVolumeEntryNameMaximumLength;
using usb_vault::volume::kVolumePathMaximumLength;
using usb_vault::volume::resolve_exported_entry_path;
using usb_vault::volume::resolve_volume_child_path;
using usb_vault::volume::resolve_volume_entry_path;

int g_failures = 0;

void require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

void require_rejects(const wchar_t drive, const std::wstring_view relative,
                     const char* const message) {
  try {
    static_cast<void>(resolve_volume_entry_path(drive, relative));
  } catch (const std::invalid_argument&) {
    return;
  } catch (...) {
    require(false, message);
    return;
  }
  require(false, message);
}

void require_resolves(const wchar_t drive, const std::wstring_view relative,
                      const std::wstring_view expected,
                      const char* const message) {
  try {
    require(resolve_volume_entry_path(drive, relative) == expected, message);
  } catch (const std::exception&) {
    require(false, message);
  }
}

void require_rejects_child(const wchar_t drive,
                           const std::wstring_view parent,
                           const std::wstring_view name,
                           const char* const message) {
  try {
    static_cast<void>(resolve_volume_child_path(drive, parent, name));
  } catch (const std::invalid_argument&) {
    return;
  } catch (...) {
    require(false, message);
    return;
  }
  require(false, message);
}

void require_resolves_child(const wchar_t drive,
                            const std::wstring_view parent,
                            const std::wstring_view name,
                            const std::wstring_view expected,
                            const char* const message) {
  try {
    require(resolve_volume_child_path(drive, parent, name) == expected,
            message);
  } catch (const std::exception&) {
    require(false, message);
  }
}

// The join is the whole point: the session names the volume, the page names a
// place inside it, and separators are normalized so the page may hold either.
void test_a_relative_path_becomes_one_absolute_path() {
  require_resolves(L'X', L"", L"X:\\",
                   "an empty relative path did not name the volume root");
  require_resolves(L'X', L"work", L"X:\\work",
                   "a single component did not join");
  require_resolves(L'X', L"work\\notes", L"X:\\work\\notes",
                   "a nested path did not join");
  require_resolves(L'X', L"work/notes", L"X:\\work\\notes",
                   "forward separators were not normalized");
  require_resolves(L'X', L"work\\notes\\", L"X:\\work\\notes",
                   "a trailing separator survived the join");
  require_resolves(L'X', L"work/", L"X:\\work",
                   "a trailing forward separator survived the join");
  require_resolves(L'X', L"工作资料\\照片", L"X:\\工作资料\\照片",
                   "a non-ASCII path was not carried through");

  // The drive letter is written as the session gave it. Nothing downstream
  // cares about its case, and rewriting it would make the resolved path stop
  // matching the letter the page was told about.
  require_resolves(L'x', L"work", L"x:\\work",
                   "a lower-case drive letter was not carried through");
}

// The session names one volume. Everything the page can say has to land inside
// it, so every way of naming somewhere else is refused by name rather than
// resolved away.
void test_traversal_and_rooted_paths_are_refused() {
  require_rejects(L'X', L"..", "a bare parent segment was accepted");
  require_rejects(L'X', L"work\\..", "a trailing parent segment was accepted");
  require_rejects(L'X', L"work\\..\\..\\Windows",
                  "a traversal out of the volume was accepted");
  require_rejects(L'X', L"..\\Windows",
                  "a leading parent segment was accepted");
  require_rejects(L'X', L"work/../notes",
                  "a parent segment written with forward separators was accepted");
  require_rejects(L'X', L".", "a bare current segment was accepted");
  require_rejects(L'X', L"work\\.\\notes",
                  "an embedded current segment was accepted");

  require_rejects(L'X', L"\\work", "a rooted path was accepted");
  require_rejects(L'X', L"/work",
                  "a rooted path written with a forward separator was accepted");
  require_rejects(L'X', L"C:\\Windows", "an absolute path was accepted");
  require_rejects(L'X', L"\\\\server\\share", "a UNC path was accepted");
  require_rejects(L'X', L"\\\\?\\C:\\Windows",
                  "an extended-length path was accepted");
  require_rejects(L'X', L"\\\\.\\PhysicalDrive0",
                  "a device namespace path was accepted");

  require_rejects(L'X', L"work\\\\notes", "an empty segment was accepted");
  require_rejects(L'X', L"\\", "a lone separator was accepted");
}

// A component is one name. Everything a name cannot carry is refused here, so
// no caller has to remember which of these the file system would have taken
// and turned into something else.
void test_a_component_that_is_not_a_name_is_refused() {
  // A colon does not make a file called `notes:stream`; it makes a stream on
  // `notes`, which is not an entry the page can see, delete, or account for.
  require_rejects(L'X', L"notes:stream",
                  "an alternate data stream was accepted");
  require_rejects(L'X', L"work\\notes:stream",
                  "a nested alternate data stream was accepted");

  require_rejects(L'X', L"note*", "a wildcard was accepted");
  require_rejects(L'X', L"note?", "a single-character wildcard was accepted");
  require_rejects(L'X', L"note\"s", "a quote was accepted");
  require_rejects(L'X', L"note<s", "an angle bracket was accepted");
  require_rejects(L'X', L"note|s", "a pipe was accepted");
  require_rejects(L'X', std::wstring(L"note\u0001s"),
                  "a control character was accepted");

  require_rejects(L'X', L"notes ", "a name ending in a space was accepted");
  require_rejects(L'X', L"notes.", "a name ending in a dot was accepted");
  require_rejects(L'X', L" notes", "a name starting with a space was accepted");
  require_rejects(L'X', L"work \\notes",
                  "a segment ending in a space was accepted");

  require_rejects(L'X', L"CON", "a reserved device name was accepted");
  require_rejects(L'X', L"con", "a lower-case device name was accepted");
  require_rejects(L'X', L"COM1.txt",
                  "a reserved device name with an extension was accepted");
  require_rejects(L'X', L"work\\NUL",
                  "a reserved device name in a segment was accepted");

  // A name that merely contains a device name is an ordinary name.
  require_resolves(L'X', L"CONTRACT", L"X:\\CONTRACT",
                   "a name that starts with a device name was refused");

  const std::wstring longest(kVolumeEntryNameMaximumLength, L'a');
  require_resolves(L'X', longest, L"X:\\" + longest,
                   "the longest allowed name was refused");
  require_rejects(L'X', longest + L"a", "a name past the limit was accepted");
}

// Renaming and creating hand over a name, not a path. Running one through the
// path resolver would let a separator inside it move the entry somewhere else,
// so a name is held to being exactly one component.
void test_a_child_name_must_be_exactly_one_component() {
  require_resolves_child(L'X', L"", L"notes", L"X:\\notes",
                         "a name at the volume root did not join");
  require_resolves_child(L'X', L"work", L"notes", L"X:\\work\\notes",
                         "a name inside a directory did not join");
  require_resolves_child(L'X', L"work\\", L"notes", L"X:\\work\\notes",
                         "a trailing separator produced a doubled one");
  require_resolves_child(L'X', L"work/drafts", L"notes",
                         L"X:\\work\\drafts\\notes",
                         "a nested parent did not join");

  require_rejects_child(L'X', L"work", L"drafts\\notes",
                        "a name holding a separator was accepted");
  require_rejects_child(L'X', L"work", L"drafts/notes",
                        "a name holding a forward separator was accepted");
  require_rejects_child(L'X', L"work", L"..",
                        "a name that walks upwards was accepted");
  require_rejects_child(L'X', L"work", L"",
                        "an empty name was accepted");
  require_rejects_child(L'X', L"work", L"notes:stream",
                        "a name naming a stream was accepted");
  require_rejects_child(L'X', L"work\\..", L"notes",
                        "a parent that walks upwards was accepted");
}

void test_only_a_drive_letter_names_a_volume() {
  require_rejects(L'1', L"work", "a digit was accepted as a drive letter");
  require_rejects(L'\0', L"work", "a null was accepted as a drive letter");
  require_rejects(L':', L"work", "a colon was accepted as a drive letter");
  require_rejects_child(L'1', L"", L"notes",
                        "a digit was accepted as a drive letter for a name");
}

// The file manager reaches the file system without the `\\?\` prefix, so a
// path past `MAX_PATH - 1` would fail far from here.
void test_the_joined_path_stays_inside_the_win32_limit() {
  // Two components, because one long enough to reach the path limit would be
  // refused as a name first.
  const std::wstring head(200, L'a');
  const std::wstring tail(kVolumePathMaximumLength - 3 - head.size() - 1, L'b');
  const std::wstring relative = head + L"\\" + tail;
  require_resolves(L'X', relative, L"X:\\" + relative,
                   "a path inside the limit was rejected");
  require_rejects(L'X', relative + L"b",
                  "a path past the Win32 limit was accepted");
  require_rejects_child(L'X', relative, L"c",
                        "a name pushing the path past the limit was accepted");
}

// 🔴 The one resolver here that is allowed to land outside a volume. Every
// other one exists to prevent exactly that, so this one is tested for what it
// still refuses at least as hard as for what it allows.
void test_an_export_location_is_a_drive_root_and_a_relative_path() {
  const auto resolves = [](const std::wstring_view directory,
                           const std::wstring_view name,
                           const std::wstring_view expected,
                           const char* const message) {
    try {
      require(resolve_exported_entry_path(directory, name) == expected,
              message);
    } catch (const std::exception&) {
      require(false, message);
    }
  };
  const auto rejects = [](const std::wstring_view directory,
                          const std::wstring_view name,
                          const char* const message) {
    try {
      static_cast<void>(resolve_exported_entry_path(directory, name));
    } catch (const std::invalid_argument&) {
      return;
    } catch (...) {
      require(false, message);
      return;
    }
    require(false, message);
  };

  resolves(L"D:\\", L"notes.txt", L"D:\\notes.txt",
           "a drive root did not join with a name");
  resolves(L"D:\\Backups", L"notes.txt", L"D:\\Backups\\notes.txt",
           "a directory did not join with a name");
  resolves(L"D:/Backups/2026/", L"notes.txt",
           L"D:\\Backups\\2026\\notes.txt",
           "forward separators and a trailing one were not normalized");
  // The exported file keeps whatever it already was. This copies a file out;
  // it does not create a new kind of file the way container creation does.
  resolves(L"D:\\Backups", L"archive.tar.gz", L"D:\\Backups\\archive.tar.gz",
           "the export renamed or retyped the file on its way out");

  rejects(L"", L"notes.txt", "an empty location was accepted");
  rejects(L"Backups", L"notes.txt", "a relative location was accepted");
  rejects(L"D:Backups", L"notes.txt", "a drive-relative location was accepted");
  rejects(L"\\\\server\\share", L"notes.txt", "a UNC location was accepted");
  rejects(L"\\\\?\\D:\\Backups", L"notes.txt",
          "a device-namespace location was accepted");
  rejects(L"D:\\..\\Windows", L"notes.txt",
          "a location that walks out of itself was accepted");
  rejects(L"D:\\Back*ups", L"notes.txt", "a wildcard was accepted in a location");
  rejects(L"D:\\Backups ", L"notes.txt",
          "a location ending in a space was accepted");
  rejects(L"1:\\Backups", L"notes.txt", "a drive letter that is not one was accepted");
  rejects(L"D:\\Backups", L"sub\\notes.txt",
          "an export name that is a path was accepted");
  rejects(L"D:\\Backups", L"", "an empty export name was accepted");
  rejects(L"D:\\Backups", L"CON", "a reserved device name was accepted");
  rejects(L"D:\\Backups", L"a:b", "an alternate data stream was accepted");

  // The Win32 limit applies to what comes out, wherever it is going.
  rejects(L"D:\\" + std::wstring(kVolumePathMaximumLength, L'd'), L"notes.txt",
          "an export past the Win32 length limit was accepted");
}

}  // namespace

int main() {
  try {
    test_a_relative_path_becomes_one_absolute_path();
    test_traversal_and_rooted_paths_are_refused();
    test_a_component_that_is_not_a_name_is_refused();
    test_a_child_name_must_be_exactly_one_component();
    test_only_a_drive_letter_names_a_volume();
    test_the_joined_path_stays_inside_the_win32_limit();
    test_an_export_location_is_a_drive_root_and_a_relative_path();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return 1;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "volume entry path tests passed\n";
  return 0;
}
