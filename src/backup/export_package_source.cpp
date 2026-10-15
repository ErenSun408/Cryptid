#include "usb_vault/backup/export_package_source.h"

#include <Windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::backup {
namespace {

constexpr std::size_t kReadBlock = kExportPackageV1ChunkSize;

[[noreturn]] void throw_last_error(const char* const operation) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), operation);
}

void check_cancelled(const std::function<bool()>& cancelled) {
  if (cancelled && cancelled()) {
    throw std::system_error(ERROR_CANCELLED, std::system_category(),
                            "the export was cancelled");
  }
}

class FindHandle final {
 public:
  explicit FindHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FindHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::FindClose(handle_);
  }
  FindHandle(const FindHandle&) = delete;
  FindHandle& operator=(const FindHandle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

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

[[nodiscard]] std::uint64_t join(const DWORD high, const DWORD low) noexcept {
  return (static_cast<std::uint64_t>(high) << 32) | low;
}

void refuse_link(const DWORD attributes) {
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    throw std::runtime_error("the export source holds a link");
  }
}

// The directory holding the exported entry, with its trailing separator.
[[nodiscard]] std::wstring source_parent(const std::wstring_view source) {
  const std::size_t separator = source.find_last_of(L"\\/");
  if (separator == std::wstring_view::npos || separator + 1 >= source.size()) {
    throw std::invalid_argument("the export source has no entry name");
  }
  return std::wstring(source.substr(0, separator + 1));
}

// What the file system actually opened, as a drive-absolute path.
[[nodiscard]] std::wstring final_path(const HANDLE handle) {
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD length = ::GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) throw_last_error("resolve an export source path");
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    buffer.resize(length + 1);
  }
  constexpr std::wstring_view prefix = L"\\\\?\\";
  if (buffer.starts_with(prefix) && !buffer.starts_with(L"\\\\?\\UNC\\")) {
    buffer.erase(0, prefix.size());
  }
  return buffer;
}

[[nodiscard]] bool same_path(const std::wstring_view left,
                             const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

// Opens one entry of the source without following a link at its end, and
// refuses it unless the file system landed exactly where the walk expected.
// The last check is what catches a directory above it swapped for a junction
// after the walk: the path string is unchanged, the opened object is not.
[[nodiscard]] HANDLE open_expected(const std::wstring& path,
                                   const std::wstring& expected,
                                   const DWORD access, const DWORD flags) {
  const HANDLE handle = ::CreateFileW(
      path.c_str(), access, FILE_SHARE_READ | (access == FILE_READ_ATTRIBUTES ? FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0),
      nullptr, OPEN_EXISTING, flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) throw_last_error("open an export source entry");
  try {
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(handle, &information) == FALSE) {
      throw_last_error("inspect an export source entry");
    }
    refuse_link(information.dwFileAttributes);
    if (!same_path(final_path(handle), expected)) {
      throw std::runtime_error("an export source entry moved during the export");
    }
  } catch (...) {
    ::CloseHandle(handle);
    throw;
  }
  return handle;
}

// The source's parent as the file system names it, with a trailing separator,
// so every expected path is built on what open_expected will report.
[[nodiscard]] std::wstring canonical_parent(const std::wstring& parent) {
  const FileHandle directory(::CreateFileW(
      parent.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (directory.get() == INVALID_HANDLE_VALUE) throw_last_error("open the export source parent");
  std::wstring result = final_path(directory.get());
  if (!result.ends_with(L'\\')) result.push_back(L'\\');
  return result;
}

void add_entry(ExportManifest& manifest, ExportManifestEntry entry) {
  if (manifest.entries.size() >= kExportManifestV1MaximumEntries) {
    throw std::length_error("the export source holds too many entries");
  }
  manifest.data_bytes += entry.size;
  manifest.entries.push_back(std::move(entry));
}

}  // namespace

ExportManifest collect_export_manifest_v1(const std::wstring_view absolute_source,
                                          const std::function<bool()>& cancelled) {
  check_cancelled(cancelled);
  const std::wstring parent = source_parent(absolute_source);
  const std::wstring top(absolute_source.substr(parent.size()));
  WIN32_FILE_ATTRIBUTE_DATA root{};
  if (::GetFileAttributesExW(std::wstring(absolute_source).c_str(),
                             GetFileExInfoStandard, &root) == FALSE) {
    throw_last_error("read the export source");
  }
  refuse_link(root.dwFileAttributes);
  const std::wstring canonical = canonical_parent(parent);

  ExportManifest manifest;
  const bool root_is_directory =
      (root.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  add_entry(manifest,
            {.kind = root_is_directory ? ExportEntryKind::directory : ExportEntryKind::file,
             .relative_path = top,
             .size = root_is_directory ? 0 : join(root.nFileSizeHigh, root.nFileSizeLow),
             .last_write_time = join(root.ftLastWriteTime.dwHighDateTime,
                                     root.ftLastWriteTime.dwLowDateTime)});

  // Depth first with an explicit stack: each directory is listed when it is
  // reached, so every directory precedes what it holds.
  std::vector<std::wstring> pending;
  if (root_is_directory) pending.push_back(top);
  while (!pending.empty()) {
    check_cancelled(cancelled);
    const std::wstring directory = std::move(pending.back());
    pending.pop_back();
    const FileHandle opened(open_expected(parent + directory, canonical + directory,
                                          FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS));
    WIN32_FIND_DATAW found{};
    const FindHandle find(::FindFirstFileExW(
        (parent + directory + L"\\*").c_str(), FindExInfoBasic, &found,
        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (find.get() == INVALID_HANDLE_VALUE) throw_last_error("list an export directory");
    std::vector<std::wstring> subdirectories;
    do {
      const std::wstring_view name(found.cFileName);
      if (name == L"." || name == L"..") continue;
      refuse_link(found.dwFileAttributes);
      const bool is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      std::wstring relative = directory + L"\\" + std::wstring(name);
      add_entry(manifest,
                {.kind = is_directory ? ExportEntryKind::directory : ExportEntryKind::file,
                 .relative_path = relative,
                 .size = is_directory ? 0 : join(found.nFileSizeHigh, found.nFileSizeLow),
                 .last_write_time = join(found.ftLastWriteTime.dwHighDateTime,
                                         found.ftLastWriteTime.dwLowDateTime)});
      if (is_directory) subdirectories.push_back(std::move(relative));
    } while (::FindNextFileW(find.get(), &found) != FALSE);
    if (::GetLastError() != ERROR_NO_MORE_FILES) throw_last_error("list an export directory");
    // Reversed so the stack visits them in listing order.
    std::ranges::reverse(subdirectories);
    for (auto& subdirectory : subdirectories) pending.push_back(std::move(subdirectory));
  }
  try {
    validate_export_manifest_v1(manifest);
  } catch (const ExportPackageRejected&) {
    throw std::runtime_error("an export source name breaks a package rule");
  }
  return manifest;
}

void write_export_package_v1(const std::wstring_view absolute_source,
                             const ExportManifest& manifest,
                             ExportPackageKeyV1 key,
                             const ExportPackageWriterV1::Sink& sink,
                             const std::function<bool()>& cancelled,
                             const ExportProgress& progress) {
  const std::wstring parent = source_parent(absolute_source);
  if (manifest.entries.empty() ||
      absolute_source.substr(parent.size()) != manifest.top_level_name()) {
    throw std::invalid_argument("the manifest does not describe this source");
  }
  const std::wstring canonical = canonical_parent(parent);
  WipedBytes encoded = encode_export_manifest_v1(manifest);
  ExportPackageWriterV1 writer(std::move(key),
                               export_package_v1_header(manifest, encoded.bytes().size()),
                               sink);
  writer.write(encoded.bytes());
  encoded.wipe();

  WipedBytes block(kReadBlock);
  const std::uint64_t total = manifest.data_bytes;
  std::uint64_t done = 0;
  if (progress) progress(done, total);
  for (const ExportManifestEntry& entry : manifest.entries) {
    if (entry.kind != ExportEntryKind::file) continue;
    check_cancelled(cancelled);
    const FileHandle file(open_expected(parent + entry.relative_path,
                                        canonical + entry.relative_path, GENERIC_READ,
                                        FILE_FLAG_SEQUENTIAL_SCAN));
    BY_HANDLE_FILE_INFORMATION information{};
    if (::GetFileInformationByHandle(file.get(), &information) == FALSE) {
      throw_last_error("inspect an export source file");
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        join(information.nFileSizeHigh, information.nFileSizeLow) != entry.size) {
      throw std::runtime_error("an export source file changed during the export");
    }
    std::uint64_t left = entry.size;
    while (left > 0) {
      check_cancelled(cancelled);
      const auto want = static_cast<DWORD>(std::min<std::uint64_t>(left, kReadBlock));
      DWORD read = 0;
      if (::ReadFile(file.get(), block.bytes().data(), want, &read, nullptr) == FALSE) {
        throw_last_error("read an export source file");
      }
      if (read == 0) throw std::runtime_error("an export source file changed during the export");
      writer.write(block.bytes().first(read));
      left -= read;
      done += read;
      if (progress) progress(done, total);
    }
    std::byte extra{};
    DWORD read = 0;
    if (::ReadFile(file.get(), &extra, 1, &read, nullptr) == FALSE) {
      throw_last_error("read an export source file");
    }
    if (read != 0) throw std::runtime_error("an export source file changed during the export");
  }
  block.wipe();
  writer.finish();
}

}  // namespace usb_vault::backup
