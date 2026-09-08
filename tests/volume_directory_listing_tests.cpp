#include "usb_vault/volume/volume_directory_listing.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using usb_vault::volume::kVolumeDirectoryEntryLimit;
using usb_vault::volume::list_volume_directory;
using usb_vault::volume::VolumeDirectoryEntry;
using usb_vault::volume::VolumeDirectoryListing;

int g_failures = 0;

void require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

template <typename Callable>
void require_throws(Callable&& callable, const char* const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::exception&) {
    return;
  }
  require(false, message);
}

[[noreturn]] void fail(const char* const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

// A directory of its own per run, so nothing here can reach anything the
// account running the tests cares about.
class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1> temp_root{};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(temp_root.size()),
                                        temp_root.data());
    if (length == 0 || length >= temp_root.size()) {
      fail("resolve the temporary directory");
    }
    std::array<wchar_t, MAX_PATH + 1> unique{};
    if (::GetTempFileNameW(temp_root.data(), L"uvd", 0, unique.data()) == 0) {
      fail("reserve a unique temporary path");
    }
    if (::DeleteFileW(unique.data()) == FALSE) {
      fail("release the reserved temporary path");
    }
    root_ = std::filesystem::path(unique.data()).concat(L".dir");
    std::filesystem::create_directories(root_);
  }

  ~TemporaryDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  TemporaryDirectory(TemporaryDirectory&&) = delete;
  TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::wstring path() const { return root_.wstring(); }

 private:
  std::filesystem::path root_;
};

void make_directory(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
}

void make_file(const std::filesystem::path& path, const std::size_t bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) fail("create a fixture file");
  const std::string filler(bytes, 'x');
  output.write(filler.data(), static_cast<std::streamsize>(filler.size()));
}

[[nodiscard]] const VolumeDirectoryEntry* find_entry(
    const VolumeDirectoryListing& listing, const std::wstring_view name) {
  for (const VolumeDirectoryEntry& entry : listing.entries) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

// A mount-point reparse point, built by hand because the structure lives in
// `ntifs.h` rather than the user-mode SDK. A junction needs no privilege,
// which is exactly why the file manager has to assume one can be there.
struct ReparseMountPoint final {
  ULONG ReparseTag;
  USHORT ReparseDataLength;
  USHORT Reserved;
  USHORT SubstituteNameOffset;
  USHORT SubstituteNameLength;
  USHORT PrintNameOffset;
  USHORT PrintNameLength;
  WCHAR PathBuffer[1];
};

[[nodiscard]] bool try_make_junction(const std::filesystem::path& link,
                                     const std::filesystem::path& target) {
  std::error_code ignored;
  std::filesystem::create_directory(link, ignored);

  const std::wstring substitute = L"\\??\\" + target.wstring();
  const std::wstring print = target.wstring();
  const std::size_t path_bytes =
      (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  const std::size_t header_bytes = offsetof(ReparseMountPoint, PathBuffer);
  std::vector<std::byte> buffer(header_bytes + path_bytes);

  auto* const reparse = reinterpret_cast<ReparseMountPoint*>(buffer.data());
  reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  reparse->ReparseDataLength = static_cast<USHORT>(
      path_bytes + (header_bytes - offsetof(ReparseMountPoint,
                                            SubstituteNameOffset)));
  reparse->Reserved = 0;
  reparse->SubstituteNameOffset = 0;
  reparse->SubstituteNameLength =
      static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
  reparse->PrintNameOffset =
      static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
  reparse->PrintNameLength =
      static_cast<USHORT>(print.size() * sizeof(wchar_t));
  std::copy(substitute.begin(), substitute.end(), reparse->PathBuffer);
  reparse->PathBuffer[substitute.size()] = L'\0';
  std::copy(print.begin(), print.end(),
            reparse->PathBuffer + substitute.size() + 1);
  reparse->PathBuffer[substitute.size() + 1 + print.size()] = L'\0';

  const HANDLE handle = ::CreateFileW(
      link.wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;

  DWORD returned = 0;
  const BOOL ok = ::DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
      static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
  ::CloseHandle(handle);
  return ok != FALSE;
}

// A drive letter whose contents actually live somewhere else. This is the
// portable stand-in for a junction that leaves the volume: both make the
// resolved path land on a different drive than the one the caller named, which
// is the single fact the escape check turns on, and this one needs neither a
// second physical volume nor any privilege.
class SubstitutedDrive final {
 public:
  explicit SubstitutedDrive(std::wstring target) : target_(std::move(target)) {
    const DWORD taken = ::GetLogicalDrives();
    for (wchar_t candidate = L'Z'; candidate >= L'D'; --candidate) {
      const DWORD bit = 1UL << static_cast<DWORD>(candidate - L'A');
      if ((taken & bit) != 0) continue;
      std::wstring device{candidate, L':'};
      if (::DefineDosDeviceW(0, device.c_str(), target_.c_str()) != FALSE) {
        letter_ = candidate;
        return;
      }
    }
  }

  ~SubstitutedDrive() noexcept {
    if (letter_ == L'\0') return;
    std::wstring device{letter_, L':'};
    ::DefineDosDeviceW(
        DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, device.c_str(),
        target_.c_str());
  }

  SubstitutedDrive(const SubstitutedDrive&) = delete;
  SubstitutedDrive& operator=(const SubstitutedDrive&) = delete;
  SubstitutedDrive(SubstitutedDrive&&) = delete;
  SubstitutedDrive& operator=(SubstitutedDrive&&) = delete;

  [[nodiscard]] bool available() const noexcept { return letter_ != L'\0'; }
  [[nodiscard]] std::wstring root() const {
    return std::wstring{letter_, L':', L'\\'};
  }

 private:
  std::wstring target_;
  wchar_t letter_{L'\0'};
};

void test_a_directory_lists_its_entries() {
  const TemporaryDirectory temporary;
  make_directory(temporary.root() / L"beta");
  make_directory(temporary.root() / L"Alpha");
  make_file(temporary.root() / L"gamma.txt", 7);
  make_file(temporary.root() / L"Delta.txt", 0);

  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path());
  require(listing.entries.size() == 4, "the listing lost or invented entries");
  require(!listing.truncated, "a small directory reported truncation");

  // Directories first, then ordinal case-insensitive order. The order has to
  // be the same on every machine, because the page indexes into it.
  require(listing.entries[0].name == L"Alpha", "directories are misordered");
  require(listing.entries[1].name == L"beta", "directories are misordered");
  require(listing.entries[2].name == L"Delta.txt", "files are misordered");
  require(listing.entries[3].name == L"gamma.txt", "files are misordered");

  require(listing.entries[0].is_directory, "a directory was reported as a file");
  require(!listing.entries[3].is_directory,
          "a file was reported as a directory");

  const VolumeDirectoryEntry* const gamma = find_entry(listing, L"gamma.txt");
  require(gamma != nullptr && gamma->size_bytes == 7,
          "a file size was not reported");
  const VolumeDirectoryEntry* const delta = find_entry(listing, L"Delta.txt");
  require(delta != nullptr && delta->size_bytes == 0,
          "an empty file was not reported as empty");

  // A directory's own record size is not what a file manager means by size.
  require(listing.entries[0].size_bytes == 0,
          "a directory reported a size");

  for (const VolumeDirectoryEntry& entry : listing.entries) {
    require(!entry.is_reparse_point,
            "an ordinary entry was called a reparse point");
  }
}

void test_dot_entries_never_appear() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"only.txt", 1);
  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path());
  require(listing.entries.size() == 1, "the dot entries were not dropped");
  require(find_entry(listing, L".") == nullptr, "the current entry appeared");
  require(find_entry(listing, L"..") == nullptr, "the parent entry appeared");
}

void test_an_empty_directory_lists_nothing() {
  const TemporaryDirectory temporary;
  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path());
  require(listing.entries.empty(), "an empty directory listed something");
  require(!listing.truncated, "an empty directory reported truncation");
}

void test_a_trailing_separator_is_accepted() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"only.txt", 1);
  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path() + L"\\");
  require(listing.entries.size() == 1,
          "a trailing separator broke the listing");
}

void test_a_missing_or_non_directory_path_is_refused() {
  const TemporaryDirectory temporary;
  const std::wstring missing = (temporary.root() / L"absent").wstring();
  require_throws([&] { static_cast<void>(list_volume_directory(missing)); },
                 "a missing directory was listed");

  const std::filesystem::path file = temporary.root() / L"plain.txt";
  make_file(file, 4);
  require_throws([&] { static_cast<void>(list_volume_directory(file.wstring())); },
                 "a file was listed as a directory");
}

void test_a_path_that_is_not_drive_absolute_is_refused() {
  require_throws([] { static_cast<void>(list_volume_directory(L"relative\\path")); },
                 "a relative path was listed");
  require_throws([] { static_cast<void>(list_volume_directory(L"\\\\server\\share")); },
                 "a UNC path was listed");
  require_throws([] { static_cast<void>(list_volume_directory(L"")); },
                 "an empty path was listed");
  require_throws([] { static_cast<void>(list_volume_directory(L"1:\\work")); },
                 "a path with no drive letter was listed");
}

// The flag is reported rather than the entry hidden: a manager that silently
// omits what Explorer shows is worse than one that shows it and refuses to go
// in. Refusing to go in is the escape check below.
void test_a_reparse_point_is_reported_but_not_hidden() {
  const TemporaryDirectory temporary;
  const std::filesystem::path target = temporary.root() / L"target";
  make_directory(target);
  make_file(target / L"inside.txt", 3);

  if (!try_make_junction(temporary.root() / L"link", target)) {
    std::cout << "  (skipped: this machine would not create a junction)\n";
    return;
  }

  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path());
  const VolumeDirectoryEntry* const link = find_entry(listing, L"link");
  require(link != nullptr, "a reparse point was hidden from the listing");
  if (link == nullptr) return;
  require(link->is_reparse_point, "a junction was not flagged");
  require(link->is_directory, "a directory junction was not a directory");

  const VolumeDirectoryEntry* const plain = find_entry(listing, L"target");
  require(plain != nullptr && !plain->is_reparse_point,
          "an ordinary directory was flagged as a reparse point");
}

// 🔴 The security core of this unit. The path still reads `<letter>:\`, and
// only the opened handle knows it landed on another volume.
void test_a_directory_that_resolves_onto_another_volume_is_refused() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"inside.txt", 2);
  const SubstitutedDrive substituted(temporary.path());
  if (!substituted.available()) {
    std::cout << "  (skipped: no free drive letter on this machine)\n";
    return;
  }

  // The same directory lists fine under the name it really has, so the refusal
  // below is about where the path resolves and not about the contents.
  const VolumeDirectoryListing direct =
      list_volume_directory(temporary.path());
  require(direct.entries.size() == 1, "the fixture directory did not list");

  require_throws([&] { static_cast<void>(list_volume_directory(substituted.root())); },
                 "a path resolving onto another volume was listed");
}

void test_the_modified_time_is_unix_milliseconds() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"now.txt", 1);
  const VolumeDirectoryListing listing =
      list_volume_directory(temporary.path());
  require(listing.entries.size() == 1, "the fixture did not list");
  if (listing.entries.empty()) return;

  FILETIME system_time{};
  ::GetSystemTimeAsFileTime(&system_time);
  const std::int64_t ticks =
      (static_cast<std::int64_t>(system_time.dwHighDateTime) << 32) |
      static_cast<std::int64_t>(system_time.dwLowDateTime);
  const std::int64_t now = (ticks - 116444736000000000LL) / 10000LL;

  const std::int64_t reported = listing.entries[0].modified_unix_ms;
  // A file written moments ago, in milliseconds since the Unix epoch. The
  // window is wide because a build machine's clock is not this test's subject.
  require(reported > now - 600000 && reported <= now + 60000,
          "the modified time is not Unix milliseconds around now");
}

void test_the_entry_limit_is_a_ceiling_not_a_page_size() {
  // Building a directory past the limit would cost more than the guarantee is
  // worth on every run, so this only pins the constant itself: the page has to
  // be able to trust that an ordinary directory is never truncated.
  require(kVolumeDirectoryEntryLimit >= 10000,
          "the entry limit is small enough to truncate ordinary directories");
}

}  // namespace

int main() {
  try {
    test_a_directory_lists_its_entries();
    test_dot_entries_never_appear();
    test_an_empty_directory_lists_nothing();
    test_a_trailing_separator_is_accepted();
    test_a_missing_or_non_directory_path_is_refused();
    test_a_path_that_is_not_drive_absolute_is_refused();
    test_a_reparse_point_is_reported_but_not_hidden();
    test_a_directory_that_resolves_onto_another_volume_is_refused();
    test_the_modified_time_is_unix_milliseconds();
    test_the_entry_limit_is_a_ceiling_not_a_page_size();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return 1;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "volume directory listing tests passed\n";
  return 0;
}
