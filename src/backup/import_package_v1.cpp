#include "usb_vault/backup/import_package_v1.h"

#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/volume/volume_entry_path.h"
#include "usb_vault/volume/volume_file_operations.h"
#include "volume/volume_containment.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace usb_vault::backup {
namespace {

constexpr std::size_t kBlock = kExportPackageV1ChunkSize;
// File system metadata per restored entry, generously: a cluster for the
// record and one for a directory index.
constexpr std::uint64_t kEntryAllowance = 8192;

[[noreturn]] void throw_win32(const DWORD code, const char* const operation) {
  throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

[[noreturn]] void throw_last_error(const char* const operation) {
  throw_win32(::GetLastError(), operation);
}

void check_cancelled(const std::function<bool()>& cancelled) {
  if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the import was cancelled");
}

[[nodiscard]] HANDLE handle_of(void* const handle) noexcept {
  return static_cast<HANDLE>(handle);
}

class FileHandle final {
 public:
  explicit FileHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FileHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[nodiscard]] std::wstring random_name() {
  std::array<unsigned char, 16> random{};
  if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, random.data(),
                                        static_cast<ULONG>(random.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    throw std::runtime_error("generate a staging name");
  }
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring name;
  for (const unsigned char byte : random) {
    name.push_back(digits[byte >> 4]);
    name.push_back(digits[byte & 0x0F]);
  }
  return name;
}

[[nodiscard]] std::wstring join(const std::wstring_view directory,
                                const std::wstring_view name) {
  if (directory.empty()) return std::wstring(name);
  return std::wstring(directory) + L"\\" + std::wstring(name);
}

void require_absent(const std::wstring& path) {
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    throw_win32(ERROR_FILE_EXISTS, "the import destination is taken");
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_FILE_NOT_FOUND) throw_win32(error, "inspect the import destination");
}

// Deletes the staging entry unless the import published it.
class Staging final {
 public:
  explicit Staging(std::wstring path) : path_(std::move(path)) {}
  ~Staging() noexcept {
    if (!created_ || published_) return;
    try {
      volume::delete_volume_entry(path_);
    } catch (...) {
    }
  }
  Staging(const Staging&) = delete;
  Staging& operator=(const Staging&) = delete;
  void created() noexcept { created_ = true; }
  void published() noexcept { published_ = true; }
  [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

 private:
  std::wstring path_;
  bool created_{false};
  bool published_{false};
};

}  // namespace

ImportPackageV1::ImportPackageV1(const std::wstring_view absolute_package) {
  const HANDLE handle = ::CreateFileW(
      std::wstring(absolute_package).c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (handle == INVALID_HANDLE_VALUE) throw_last_error("open the import package");
  handle_ = handle;
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle, &information) == FALSE) {
    const DWORD error = ::GetLastError();
    ::CloseHandle(handle);
    throw_win32(error, "inspect the import package");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    ::CloseHandle(handle);
    throw std::runtime_error("the import package is not a file");
  }
  size_ = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) |
          information.nFileSizeLow;
}

ImportPackageV1::~ImportPackageV1() noexcept { ::CloseHandle(handle_of(handle_)); }

namespace {

[[nodiscard]] ExportPackageReaderV1 open_reader(void* const handle, const std::uint64_t size,
                                                const security::SecureBuffer& password) {
  LARGE_INTEGER start{};
  if (::SetFilePointerEx(handle_of(handle), start, nullptr, FILE_BEGIN) == FALSE) {
    throw_last_error("rewind the import package");
  }
  return ExportPackageReaderV1(
      size,
      [handle](const std::span<std::byte> out) {
        std::span<std::byte> rest = out;
        while (!rest.empty()) {
          const auto want = static_cast<DWORD>(std::min<std::size_t>(rest.size(), 1U << 30));
          DWORD read = 0;
          if (::ReadFile(handle_of(handle), rest.data(), want, &read, nullptr) == FALSE) {
            throw_last_error("read the import package");
          }
          if (read == 0) throw std::runtime_error("the import package ended early");
          rest = rest.subspan(read);
        }
      },
      [&password](const ExportPackageV1Salt& salt) {
        return ExportPackageKeyV1::derive(password, salt);
      });
}

}  // namespace

const ExportManifest& ImportPackageV1::verify(const security::SecureBuffer& password_utf8,
                                              const std::function<bool()>& cancelled,
                                              const ImportProgress& progress) {
  verified_ = false;
  check_cancelled(cancelled);
  ExportPackageReaderV1 reader = open_reader(handle_, size_, password_utf8);
  manifest_ = verify_export_package_v1(
      reader, cancelled, [&](const std::uint64_t done, const std::uint64_t total) {
        if (progress) progress(1, done, total);
      });
  verified_ = true;
  return manifest_;
}

std::uint64_t ImportPackageV1::required_bytes(const std::uint64_t cluster_bytes) const {
  if (!verified_) throw std::logic_error("the import package has not been verified");
  if (cluster_bytes == 0) throw std::invalid_argument("a cluster has a size");
  std::uint64_t total = 0;
  for (const ExportManifestEntry& entry : manifest_.entries) {
    // Every file takes whole clusters, and every entry at least its record.
    total += (entry.size + cluster_bytes - 1) / cluster_bytes * cluster_bytes;
    total += std::max(cluster_bytes, kEntryAllowance);
  }
  return total;
}

void ImportPackageV1::set_restored_name(const std::wstring_view name) {
  if (!name.empty()) static_cast<void>(volume::resolve_volume_child_path(L'C', L"", name));
  restored_name_ = name;
}

std::wstring ImportPackageV1::write_into(const security::SecureBuffer& password_utf8,
                                         const wchar_t drive_letter,
                                         const std::wstring_view relative_directory,
                                         const std::function<bool()>& cancelled,
                                         const ImportProgress& progress) {
  if (!verified_) throw std::logic_error("the import package has not been verified");
  const std::wstring original_top = manifest_.top_level_name();
  const std::wstring top = restored_name_.empty() ? original_top : restored_name_;
  const std::wstring final_path =
      volume::resolve_volume_child_path(drive_letter, relative_directory, top);
  const std::wstring staging_name = random_name();
  Staging staging(volume::resolve_volume_child_path(drive_letter, relative_directory, staging_name));

  // Every restored path, checked under both its staging and its final name
  // before anything is created: a path too long for this directory fails
  // here, not half way through the tree.
  std::vector<std::wstring> targets;
  targets.reserve(manifest_.entries.size());
  for (const ExportManifestEntry& entry : manifest_.entries) {
    const std::wstring rest = entry.relative_path.substr(original_top.size());
    static_cast<void>(volume::resolve_volume_entry_path(
        drive_letter, join(relative_directory, top + rest)));
    targets.push_back(volume::resolve_volume_entry_path(
        drive_letter, join(relative_directory, staging_name + rest)));
  }
  require_absent(final_path);
  volume::require_parent_stays_on_volume(staging.path(), drive_letter);
  ULARGE_INTEGER available{};
  const std::wstring root{drive_letter, L':', L'\\'};
  if (::GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) == FALSE) {
    throw_last_error("measure the import destination");
  }
  DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, clusters = 0;
  if (::GetDiskFreeSpaceW(root.c_str(), &sectors_per_cluster, &bytes_per_sector,
                          &free_clusters, &clusters) == FALSE) {
    throw_last_error("measure the import destination clusters");
  }
  const std::uint64_t cluster =
      std::max<std::uint64_t>(static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector, 512);
  if (available.QuadPart < required_bytes(cluster)) {
    throw_win32(ERROR_DISK_FULL, "the import does not fit");
  }

  check_cancelled(cancelled);
  ExportPackageReaderV1 reader = open_reader(handle_, size_, password_utf8);
  // The second reading must describe exactly what the first one verified.
  if (read_export_manifest_v1(reader).entries != manifest_.entries) {
    throw ExportPackageRejected();
  }

  WipedBytes block(kBlock);
  const std::uint64_t total = manifest_.data_bytes;
  std::uint64_t done = 0;
  if (progress) progress(2, done, total);
  for (std::size_t index = 0; index < manifest_.entries.size(); ++index) {
    check_cancelled(cancelled);
    const ExportManifestEntry& entry = manifest_.entries[index];
    const std::wstring& target = targets[index];
    if (entry.kind == ExportEntryKind::directory) {
      if (::CreateDirectoryW(target.c_str(), nullptr) == FALSE) {
        throw_last_error("create an imported directory");
      }
      if (index == 0) {
        staging.created();
        if (::SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN) == FALSE) {
          throw_last_error("hide the import staging directory");
        }
      }
      continue;
    }
    const FileHandle file(::CreateFileW(
        target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        index == 0 ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) throw_last_error("create an imported file");
    if (index == 0) staging.created();
    std::uint64_t left = entry.size;
    while (left > 0) {
      check_cancelled(cancelled);
      const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(left, kBlock));
      reader.read(block.bytes().first(size));
      DWORD written = 0;
      if (::WriteFile(file.get(), block.bytes().data(), static_cast<DWORD>(size), &written,
                      nullptr) == FALSE) {
        throw_last_error("write an imported file");
      }
      if (written != size) throw_win32(ERROR_WRITE_FAULT, "write an imported file");
      left -= size;
      done += size;
      if (progress) progress(2, done, total);
    }
    FILETIME modified{};
    modified.dwLowDateTime = static_cast<DWORD>(entry.last_write_time & 0xFFFFFFFFU);
    modified.dwHighDateTime = static_cast<DWORD>(entry.last_write_time >> 32);
    static_cast<void>(::SetFileTime(file.get(), nullptr, nullptr, &modified));
  }
  block.wipe();
  if (reader.remaining() != 0) throw ExportPackageRejected();
  check_cancelled(cancelled);

  if (::SetFileAttributesW(staging.path().c_str(), FILE_ATTRIBUTE_NORMAL) == FALSE) {
    throw_last_error("reveal the imported entry");
  }
  if (::MoveFileExW(staging.path().c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
    const DWORD error = ::GetLastError();
    throw_win32(error == ERROR_ALREADY_EXISTS ? ERROR_FILE_EXISTS : error,
                "publish the imported entry");
  }
  staging.published();
  return top;
}

}  // namespace usb_vault::backup
