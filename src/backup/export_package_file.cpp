#include "usb_vault/backup/export_package_file.h"

#include "usb_vault/backup/export_package_source.h"

#include "usb_vault/volume/volume_file_operations.h"
#include "volume/volume_containment.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace usb_vault::backup {
namespace {

[[noreturn]] void throw_win32(const DWORD code, const char* const operation) {
  throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}

[[noreturn]] void throw_last_error(const char* const operation) {
  throw_win32(::GetLastError(), operation);
}

[[nodiscard]] std::wstring parent_of(const std::wstring_view path) {
  const std::size_t separator = path.find_last_of(L'\\');
  if (separator == std::wstring_view::npos || separator + 1 >= path.size()) {
    throw std::invalid_argument("the export destination has no file name");
  }
  return std::wstring(path.substr(0, separator + 1));
}

// A random name beside the destination that says nothing about what it is.
[[nodiscard]] std::wstring temporary_name(const std::wstring& directory) {
  std::array<unsigned char, 16> random{};
  if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, random.data(),
                                        static_cast<ULONG>(random.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    throw std::runtime_error("generate a temporary export name");
  }
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring name = directory;
  for (const unsigned char byte : random) {
    name.push_back(digits[byte >> 4]);
    name.push_back(digits[byte & 0x0F]);
  }
  return name;
}

void require_absent(const std::wstring& path) {
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    throw_win32(ERROR_FILE_EXISTS, "the export destination already exists");
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_FILE_NOT_FOUND) throw_win32(error, "inspect the export destination");
}

// The temporary file, deleted on every path except a successful publish.
class TemporaryPackage final {
 public:
  explicit TemporaryPackage(std::wstring path) : path_(std::move(path)) {
    handle_ = ::CreateFileW(path_.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw_last_error("create the temporary export file");
  }
  ~TemporaryPackage() noexcept {
    close();
    if (!published_) static_cast<void>(::DeleteFileW(path_.c_str()));
  }
  TemporaryPackage(const TemporaryPackage&) = delete;
  TemporaryPackage& operator=(const TemporaryPackage&) = delete;

  void write(const std::span<const std::byte> bytes) {
    std::span<const std::byte> rest = bytes;
    while (!rest.empty()) {
      const auto want = static_cast<DWORD>(std::min<std::size_t>(rest.size(), 1U << 30));
      DWORD written = 0;
      if (::WriteFile(handle_, rest.data(), want, &written, nullptr) == FALSE) {
        throw_last_error("write the export package");
      }
      if (written == 0) throw_win32(ERROR_WRITE_FAULT, "write the export package");
      rest = rest.subspan(written);
    }
  }

  // Flushes, drops the hidden attribute the file was created with, and
  // renames it to `destination` through the still-open, unshared handle, so
  // nothing else can open it between the last write and the rename. An
  // occupied destination is never replaced.
  void publish(const std::wstring& destination) {
    if (::FlushFileBuffers(handle_) == FALSE) throw_last_error("flush the export package");
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (::SetFileInformationByHandle(handle_, FileBasicInfo, &basic, sizeof(basic)) == FALSE) {
      throw_last_error("reset the export package attributes");
    }
    const std::size_t name_bytes = destination.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* const rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::copy(destination.begin(), destination.end(), rename->FileName);
    if (::SetFileInformationByHandle(handle_, FileRenameInfo, rename,
                                     static_cast<DWORD>(buffer.size())) == FALSE) {
      const DWORD error = ::GetLastError();
      throw_win32(error == ERROR_ALREADY_EXISTS ? ERROR_FILE_EXISTS : error,
                  "publish the export package");
    }
    published_ = true;
    close();
  }

 private:
  void close() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

  std::wstring path_;
  HANDLE handle_{INVALID_HANDLE_VALUE};
  bool published_{false};
};

}  // namespace

void export_package_to_file_v1(const std::wstring_view absolute_source,
                               const std::wstring_view absolute_destination,
                               const security::SecureBuffer& password_utf8,
                               const std::function<bool()>& cancelled,
                               const ExportProgress& progress) {
  const std::wstring destination(absolute_destination);
  const std::wstring directory = parent_of(destination);
  require_absent(destination);

  const ExportManifest manifest = collect_export_manifest_v1(absolute_source, cancelled);
  const std::uint64_t manifest_bytes = encode_export_manifest_v1(manifest).bytes().size();
  const std::uint64_t size =
      export_package_v1_size(export_package_v1_header(manifest, manifest_bytes));
  ULARGE_INTEGER available{};
  if (::GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr) == FALSE) {
    throw_last_error("measure the export destination");
  }
  if (available.QuadPart < size) throw_win32(ERROR_DISK_FULL, "the export package does not fit");

  if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the export was cancelled");
  ExportPackageKeyV1 key =
      ExportPackageKeyV1::derive(password_utf8, generate_export_package_v1_salt());
  TemporaryPackage temporary(temporary_name(directory));
  write_export_package_v1(
      absolute_source, manifest, std::move(key),
      [&](const std::span<const std::byte> bytes) { temporary.write(bytes); },
      cancelled, progress);
  if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the export was cancelled");
  temporary.publish(destination);
}

void export_selection_to_file_v1(const std::span<const ExportSelectionSourceV1> sources,
    const std::wstring_view absolute_destination, const std::wstring_view folder_name,
    const bool encrypted, const security::SecureBuffer& password_utf8,
    const std::function<bool()>& cancelled, const ExportProgress& progress) {
  const std::wstring destination(absolute_destination);
  volume::require_parent_stays_on_volume(destination, volume::require_drive_absolute(destination));
  const auto directory = parent_of(destination);
  require_absent(destination);
  const auto plan = collect_export_selection_v1(sources, folder_name, cancelled);
  const auto required = encrypted ? export_package_v1_size(export_package_v1_header(
      plan.manifest, encode_export_manifest_v1(plan.manifest).bytes().size())) : plan.manifest.data_bytes;
  ULARGE_INTEGER available{};
  if (::GetDiskFreeSpaceExW(directory.c_str(), &available, nullptr, nullptr) == FALSE)
    throw_last_error("measure the export destination");
  if (available.QuadPart < required) throw_win32(ERROR_DISK_FULL, "the export does not fit");
  if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the export was cancelled");
  if (encrypted) {
    auto key = ExportPackageKeyV1::derive(password_utf8, generate_export_package_v1_salt());
    TemporaryPackage temporary(temporary_name(directory));
    write_export_selection_package_v1(plan, std::move(key),
        [&](const std::span<const std::byte> bytes) { temporary.write(bytes); }, cancelled, progress);
    if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the export was cancelled");
    temporary.publish(destination);
    return;
  }
  const auto staging = temporary_name(directory);
  if (::CreateDirectoryW(staging.c_str(), nullptr) == FALSE)
    throw_last_error("create the plaintext export staging directory");
  HANDLE pin = INVALID_HANDLE_VALUE;
  bool published = false;
  try {
    pin = ::CreateFileW(staging.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (pin == INVALID_HANDLE_VALUE) throw_last_error("pin the export staging directory");
    write_export_selection_directory_v1(plan, staging, cancelled, progress);
    if (cancelled && cancelled()) throw_win32(ERROR_CANCELLED, "the export was cancelled");
    const auto name_bytes = destination.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->ReplaceIfExists = FALSE; rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::copy(destination.begin(), destination.end(), rename->FileName);
    if (::SetFileInformationByHandle(pin, FileRenameInfo, rename,
        static_cast<DWORD>(buffer.size())) == FALSE) throw_last_error("publish the export directory");
    published = true;
    ::CloseHandle(pin); pin = INVALID_HANDLE_VALUE;
  } catch (...) {
    if (pin != INVALID_HANDLE_VALUE) ::CloseHandle(pin);
    if (!published) { try { volume::delete_volume_entry(staging); } catch (...) {} }
    throw;
  }
}

}  // namespace usb_vault::backup
