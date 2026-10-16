#include "usb_vault/backup/export_package_source.h"
#include "usb_vault/volume/volume_entry_path.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <memory>
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
  if (entry.size > std::numeric_limits<std::uint64_t>::max() - manifest.data_bytes)
    throw std::length_error("the export data size is too large");
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

ExportSelectionPlanV1 collect_export_selection_v1(
    const std::span<const ExportSelectionSourceV1> sources, const std::wstring_view folder_name,
    const std::function<bool()>& cancelled) {
  if (sources.empty() || sources.size() > kExportManifestV1MaximumEntries)
    throw std::invalid_argument("an export selection must contain entries");
  // Without a folder name the single source is itself the top-level entry.
  const bool wrapped = !folder_name.empty();
  if (!wrapped && sources.size() != 1)
    throw std::invalid_argument("an unwrapped export takes exactly one entry");
  ExportSelectionPlanV1 plan;
  if (wrapped) {
    static_cast<void>(volume::resolve_volume_child_path(L'C', L"", folder_name));
    add_entry(plan.manifest, {.kind = ExportEntryKind::directory, .relative_path = std::wstring(folder_name)});
    plan.files.push_back({});
  }
  std::vector<std::wstring> names;
  for (const auto& source : sources) {
    check_cancelled(cancelled);
    if (source.output_name.empty()) throw std::invalid_argument("empty export entry name");
    static_cast<void>(volume::resolve_volume_child_path(L'C', L"", source.output_name));
    const auto parent = source_parent(source.absolute_source);
    const auto canonical = canonical_parent(parent);
    if (!same_path(parent, canonical))
      throw std::runtime_error("an export source parent contains a link or redirected path");
    const auto part = collect_export_manifest_v1(source.absolute_source, cancelled);
    const auto original_top = part.top_level_name();
    // Selected entries sit side by side in the folder, so a name already
    // taken there gets " (2)", " (3)" and so on, before a file's extension.
    std::wstring name = source.output_name;
    const auto dot = part.entries.front().kind == ExportEntryKind::file
        ? name.find_last_of(L'.') : std::wstring::npos;
    const auto stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    const auto extension = stem.size() == name.size() ? std::wstring() : name.substr(stem.size());
    for (unsigned int ordinal = 2; std::ranges::any_of(names,
             [&](const auto& taken) { return same_path(taken, name); }); ++ordinal) {
      name = stem + L" (" + std::to_wstring(ordinal) + L")" + extension;
    }
    static_cast<void>(volume::resolve_volume_child_path(L'C', L"", name));
    names.push_back(name);
    const auto top = wrapped ? std::wstring(folder_name) + L"\\" + name : name;
    for (const auto& entry : part.entries) {
      auto projected = entry;
      projected.relative_path = top + entry.relative_path.substr(original_top.size());
      add_entry(plan.manifest, std::move(projected));
      plan.files.push_back(entry.kind == ExportEntryKind::file
        ? ExportSelectionFileV1{parent + entry.relative_path, canonical + entry.relative_path}
        : ExportSelectionFileV1{});
    }
  }
  try { validate_export_manifest_v1(plan.manifest); }
  catch (const ExportPackageRejected&) {
    throw std::invalid_argument("the export selection contains conflicting or invalid paths");
  }
  return plan;
}

void write_export_selection_directory_v1(const ExportSelectionPlanV1& plan,
    const std::wstring_view staging_directory, const std::function<bool()>& cancelled,
    const ExportProgress& progress) {
  validate_export_manifest_v1(plan.manifest);
  if (plan.files.size() != plan.manifest.entries.size())
    throw std::invalid_argument("the export selection plan is inconsistent");
  const std::wstring staging(staging_directory);
  const auto parent = source_parent(staging);
  const FileHandle root(open_expected(staging, canonical_parent(parent) + staging.substr(parent.size()),
      FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS));
  const std::wstring expected = final_path(root.get()) + L"\\";
  std::vector<std::unique_ptr<FileHandle>> directories;
  WipedBytes block(kReadBlock);
  std::uint64_t done = 0;
  const auto total = plan.manifest.data_bytes;
  if (progress) progress(done, total);
  for (std::size_t index = 1; index < plan.manifest.entries.size(); ++index) {
    check_cancelled(cancelled);
    const auto& entry = plan.manifest.entries[index];
    const auto suffix = entry.relative_path.substr(plan.manifest.top_level_name().size() + 1);
    const auto destination = staging + L"\\" + suffix;
    const auto expected_destination = expected + suffix;
    if (entry.kind == ExportEntryKind::directory) {
      if (::CreateDirectoryW(destination.c_str(), nullptr) == FALSE)
        throw_last_error("create an export directory");
      // Retained directory handles prevent junction replacement during writes.
      const HANDLE handle = ::CreateFileW(destination.c_str(), FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) throw_last_error("pin an export directory");
      auto pin = std::make_unique<FileHandle>(handle);
      BY_HANDLE_FILE_INFORMATION information{};
      if (::GetFileInformationByHandle(handle, &information) == FALSE)
        throw_last_error("inspect an export directory");
      refuse_link(information.dwFileAttributes);
      if (!same_path(final_path(handle), expected_destination))
        throw std::runtime_error("an export directory moved");
      directories.push_back(std::move(pin));
      continue;
    }
    const auto& physical = plan.files[index];
    const FileHandle input(open_expected(physical.absolute_path, physical.expected_path,
        GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(input.get(), &size) == FALSE) throw_last_error("measure an export source");
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != entry.size)
      throw std::runtime_error("an export source changed");
    const FileHandle output(::CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE) throw_last_error("create an export file");
    if (!same_path(final_path(output.get()), expected_destination))
      throw std::runtime_error("an export destination moved");
    auto left = entry.size;
    while (left != 0) {
      check_cancelled(cancelled);
      DWORD read = 0;
      if (::ReadFile(input.get(), block.bytes().data(),
          static_cast<DWORD>(std::min<std::uint64_t>(left, kReadBlock)), &read, nullptr) == FALSE)
        throw_last_error("read an export source");
      if (read == 0) throw std::runtime_error("an export source changed");
      DWORD written = 0;
      if (::WriteFile(output.get(), block.bytes().data(), read, &written, nullptr) == FALSE)
        throw_last_error("write an export file");
      if (written != read) throw std::runtime_error("an export write was incomplete");
      left -= read; done += read;
      if (progress) progress(done, total);
    }
    std::byte extra{}; DWORD extra_read = 0;
    if (::ReadFile(input.get(), &extra, 1, &extra_read, nullptr) == FALSE)
      throw_last_error("finish reading an export source");
    if (extra_read != 0) throw std::runtime_error("an export source changed");
    FILETIME modified{static_cast<DWORD>(entry.last_write_time),
        static_cast<DWORD>(entry.last_write_time >> 32)};
    if (::SetFileTime(output.get(), nullptr, nullptr, &modified) == FALSE ||
        ::FlushFileBuffers(output.get()) == FALSE) throw_last_error("finish an export file");
  }
}

void write_export_selection_file_v1(const ExportSelectionPlanV1& plan,
    const ExportPackageWriterV1::Sink& sink, const std::function<bool()>& cancelled,
    const ExportProgress& progress) {
  validate_export_manifest_v1(plan.manifest);
  if (plan.files.size() != 1 || plan.manifest.entries.size() != 1 ||
      plan.manifest.entries.front().kind != ExportEntryKind::file)
    throw std::invalid_argument("the export selection plan is not a single file");
  const auto& entry = plan.manifest.entries.front();
  const auto& physical = plan.files.front();
  const FileHandle input(open_expected(physical.absolute_path, physical.expected_path,
      GENERIC_READ, FILE_FLAG_SEQUENTIAL_SCAN));
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(input.get(), &information) == FALSE)
    throw_last_error("inspect an export source file");
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      join(information.nFileSizeHigh, information.nFileSizeLow) != entry.size)
    throw std::runtime_error("an export source changed");
  WipedBytes block(kReadBlock);
  std::uint64_t done = 0;
  if (progress) progress(done, entry.size);
  while (done != entry.size) {
    check_cancelled(cancelled);
    DWORD read = 0;
    if (::ReadFile(input.get(), block.bytes().data(),
        static_cast<DWORD>(std::min<std::uint64_t>(entry.size - done, kReadBlock)), &read, nullptr) == FALSE)
      throw_last_error("read an export source");
    if (read == 0) throw std::runtime_error("an export source changed");
    sink(block.bytes().first(read));
    done += read;
    if (progress) progress(done, entry.size);
  }
  std::byte extra{}; DWORD extra_read = 0;
  if (::ReadFile(input.get(), &extra, 1, &extra_read, nullptr) == FALSE)
    throw_last_error("finish reading an export source");
  if (extra_read != 0) throw std::runtime_error("an export source changed");
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
  ExportSelectionPlanV1 plan;
  plan.manifest = manifest;
  for (const auto& entry : manifest.entries) {
    plan.files.push_back({parent + entry.relative_path, canonical + entry.relative_path});
  }
  write_export_selection_package_v1(plan, std::move(key), sink, cancelled, progress);
}

void write_export_selection_package_v1(const ExportSelectionPlanV1& plan,
    ExportPackageKeyV1 key, const ExportPackageWriterV1::Sink& sink,
    const std::function<bool()>& cancelled, const ExportProgress& progress) {
  const auto& manifest = plan.manifest;
  if (plan.files.size() != manifest.entries.size())
    throw std::invalid_argument("the export selection plan is inconsistent");
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
  for (std::size_t index = 0; index < manifest.entries.size(); ++index) {
    const ExportManifestEntry& entry = manifest.entries[index];
    if (entry.kind != ExportEntryKind::file) continue;
    check_cancelled(cancelled);
    const auto& source = plan.files[index];
    const FileHandle file(open_expected(source.absolute_path,
                                        source.expected_path, GENERIC_READ,
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
