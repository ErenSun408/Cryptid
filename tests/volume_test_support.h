#pragma once

// Fixtures shared by the volume storage-layer tests. They build the shapes the
// file manager has to survive -- a directory of its own, a junction, a drive
// letter whose contents live elsewhere -- and none of them need privilege, so
// every test that uses them runs on an ordinary account.

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault_test {

// A directory of its own per fixture, so nothing here can reach anything the
// account running the tests cares about.
class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1> temp_root{};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(temp_root.size()),
                                        temp_root.data());
    if (length == 0 || length >= temp_root.size()) {
      throw std::runtime_error("resolve the temporary directory");
    }
    std::array<wchar_t, MAX_PATH + 1> unique{};
    if (::GetTempFileNameW(temp_root.data(), L"uvv", 0, unique.data()) == 0) {
      throw std::runtime_error("reserve a unique temporary path");
    }
    if (::DeleteFileW(unique.data()) == FALSE) {
      throw std::runtime_error("release the reserved temporary path");
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
  [[nodiscard]] std::wstring child(const std::wstring& relative) const {
    return (root_ / relative).wstring();
  }

 private:
  std::filesystem::path root_;
};

inline void make_directory(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
}

inline void make_file(const std::filesystem::path& path,
                      const std::size_t bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) throw std::runtime_error("create a fixture file");
  const std::string filler(bytes, 'x');
  output.write(filler.data(), static_cast<std::streamsize>(filler.size()));
}

// Named apart from `std::filesystem::exists` on purpose: an unqualified call
// with a `path` argument finds that one by argument-dependent lookup, and the
// two would be ambiguous at every call site.
[[nodiscard]] inline bool path_exists(const std::filesystem::path& path) {
  std::error_code ignored;
  return std::filesystem::exists(path, ignored);
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

[[nodiscard]] inline bool try_make_junction(
    const std::filesystem::path& link, const std::filesystem::path& target) {
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
      path_bytes +
      (header_bytes - offsetof(ReparseMountPoint, SubstituteNameOffset)));
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
      const std::wstring device{candidate, L':'};
      if (::DefineDosDeviceW(0, device.c_str(), target_.c_str()) != FALSE) {
        letter_ = candidate;
        return;
      }
    }
  }

  ~SubstitutedDrive() noexcept {
    if (letter_ == L'\0') return;
    const std::wstring device{letter_, L':'};
    ::DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                       device.c_str(), target_.c_str());
  }

  SubstitutedDrive(const SubstitutedDrive&) = delete;
  SubstitutedDrive& operator=(const SubstitutedDrive&) = delete;
  SubstitutedDrive(SubstitutedDrive&&) = delete;
  SubstitutedDrive& operator=(SubstitutedDrive&&) = delete;

  [[nodiscard]] bool available() const noexcept { return letter_ != L'\0'; }
  [[nodiscard]] std::wstring root() const {
    return std::wstring{letter_, L':', L'\\'};
  }
  [[nodiscard]] std::wstring child(const std::wstring& relative) const {
    return root() + relative;
  }

 private:
  std::wstring target_;
  wchar_t letter_{L'\0'};
};

}  // namespace usb_vault_test
