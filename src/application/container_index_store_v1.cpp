#include "usb_vault/application/container_index_store_v1.h"

#include "usb_vault/application/container_index_v1.h"

#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::application {
namespace {

constexpr std::wstring_view kProductDirectory = L"usb-vault";
constexpr std::wstring_view kIndexFileName = L"container_index.bin";
constexpr std::wstring_view kHexDigits = L"0123456789abcdef";
constexpr std::size_t kStagingNoiseSize = 8;

[[noreturn]] void throw_last_error(const DWORD error,
                                   const char *const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

[[nodiscard]] DWORD checked_dword(const std::size_t size) {
  if (size > std::numeric_limits<DWORD>::max()) {
    return std::numeric_limits<DWORD>::max();
  }
  return static_cast<DWORD>(size);
}

class FileHandle final {
public:
  explicit FileHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FileHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(handle_));
    }
  }

  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
  HANDLE handle_;
};

// Removes the half-written file if anything between creating it and publishing
// it throws. A leftover staging file beside the index would be its own small
// signal, and would keep accumulating.
class StagingFile final {
public:
  explicit StagingFile(std::wstring path) noexcept : path_(std::move(path)) {}
  ~StagingFile() noexcept {
    if (armed_) {
      static_cast<void>(::DeleteFileW(path_.c_str()));
    }
  }

  StagingFile(const StagingFile &) = delete;
  StagingFile &operator=(const StagingFile &) = delete;

  [[nodiscard]] const std::wstring &path() const noexcept { return path_; }
  void release() noexcept { armed_ = false; }

private:
  std::wstring path_;
  bool armed_{true};
};

[[nodiscard]] std::wstring staging_path_for(const std::wstring_view path) {
  std::array<std::byte, kStagingNoiseSize> noise{};
  const NTSTATUS status = ::BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(noise.data()),
      static_cast<ULONG>(noise.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            "name the container index staging file");
  }

  std::wstring result(path);
  result += L'.';
  for (const std::byte value : noise) {
    const auto number = std::to_integer<unsigned int>(value);
    result += kHexDigits[number >> 4U];
    result += kHexDigits[number & 0x0fU];
  }
  result += L".tmp";
  return result;
}

[[nodiscard]] std::wstring parent_directory(const std::wstring_view path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring_view::npos) {
    return {};
  }
  return std::wstring(path.substr(0, separator));
}

void create_parent_directory(const std::wstring_view path) {
  const std::wstring parent = parent_directory(path);
  // A bare drive is already there and cannot be created.
  if (parent.empty() || parent.back() == L':') {
    return;
  }
  if (::CreateDirectoryW(parent.c_str(), nullptr) != FALSE) {
    return;
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_ALREADY_EXISTS) {
    throw_last_error(error, "create the container index directory");
  }
}

[[nodiscard]] std::optional<std::uint64_t>
file_size_if_present(const std::wstring &path) {
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                             &attributes) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return std::nullopt;
    }
    throw_last_error(error, "inspect the container index file");
  }
  if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    throw std::runtime_error("the container index path is a directory");
  }
  return (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32U) |
         attributes.nFileSizeLow;
}

void write_new_file(const std::wstring &path,
                    const std::span<const std::byte> contents) {
  const FileHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
  if (!file.valid()) {
    throw_last_error(::GetLastError(),
                     "create the container index staging file");
  }

  std::size_t total = 0;
  while (total < contents.size()) {
    DWORD written = 0;
    if (::WriteFile(file.get(), contents.data() + total,
                    checked_dword(contents.size() - total), &written,
                    nullptr) == FALSE) {
      throw_last_error(::GetLastError(),
                       "write the container index staging file");
    }
    if (written == 0) {
      throw std::runtime_error(
          "the container index staging file stopped accepting bytes");
    }
    total += written;
  }

  // The rename below is what makes the change atomic, but only once the bytes
  // it renames are actually on the disk rather than in the cache.
  if (::FlushFileBuffers(file.get()) == FALSE) {
    throw_last_error(::GetLastError(),
                     "flush the container index staging file");
  }
}

// Returns false when another process published one first, which is a result
// and not a failure: whichever full-size random file wins is equally good.
[[nodiscard]] bool publish_new(const std::wstring &staging,
                               const std::wstring &path) {
  if (::MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) !=
      FALSE) {
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
    return false;
  }
  throw_last_error(error, "publish the container index file");
}

void publish_replacement(const std::wstring &staging,
                         const std::wstring &path) {
  // ReplaceFileW rather than a move, because it carries the replaced file's
  // creation time onto the replacement. A move would stamp the index as newly
  // created every time an entry changed, which would say through the file
  // system what the sealed slots do not.
  if (::ReplaceFileW(path.c_str(), staging.c_str(), nullptr,
                     REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE) {
    return;
  }
  DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND) {
    // Nothing to replace. Keep the write rather than lose it.
    if (::MoveFileExW(staging.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
        FALSE) {
      return;
    }
    error = ::GetLastError();
  }
  throw_last_error(error, "replace the container index file");
}

void require_whole_file(const std::size_t size) {
  if (size != kContainerIndexV1FileSize) {
    throw std::invalid_argument(
        "the container index buffer is not a whole file");
  }
}

} // namespace

std::wstring container_index_v1_default_path() {
  wchar_t *raw_folder = nullptr;
  const HRESULT result = ::SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &raw_folder);
  if (FAILED(result) || raw_folder == nullptr) {
    if (raw_folder != nullptr) {
      ::CoTaskMemFree(raw_folder);
    }
    throw std::runtime_error("resolve the local application data folder");
  }
  std::wstring path(raw_folder);
  ::CoTaskMemFree(raw_folder);

  path += L'\\';
  path += kProductDirectory;
  path += L'\\';
  path += kIndexFileName;
  return path;
}

bool ensure_container_index_v1_file(const std::wstring_view path) {
  const std::wstring target(path);
  if (const std::optional<std::uint64_t> present = file_size_if_present(target);
      present.has_value()) {
    if (*present != kContainerIndexV1FileSize) {
      throw std::runtime_error(
          "the container index file has an unexpected size");
    }
    return false;
  }

  create_parent_directory(target);

  std::vector<std::byte> contents(kContainerIndexV1FileSize);
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    fill_container_index_slot_v1(
        container_index_v1_mutable_slot(contents, index));
  }

  StagingFile staging(staging_path_for(target));
  write_new_file(staging.path(), contents);
  if (!publish_new(staging.path(), target)) {
    return false;
  }
  staging.release();
  return true;
}

std::vector<std::byte>
read_container_index_v1_file(const std::wstring_view path) {
  const std::wstring target(path);
  const FileHandle file(::CreateFileW(target.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    throw_last_error(::GetLastError(), "open the container index file");
  }

  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == FALSE) {
    throw_last_error(::GetLastError(), "size the container index file");
  }
  if (size.QuadPart != static_cast<LONGLONG>(kContainerIndexV1FileSize)) {
    throw std::runtime_error("the container index file has an unexpected size");
  }

  std::vector<std::byte> contents(kContainerIndexV1FileSize);
  std::size_t total = 0;
  while (total < contents.size()) {
    DWORD read = 0;
    if (::ReadFile(file.get(), contents.data() + total,
                   checked_dword(contents.size() - total), &read,
                   nullptr) == FALSE) {
      throw_last_error(::GetLastError(), "read the container index file");
    }
    if (read == 0) {
      throw std::runtime_error("the container index file ended early");
    }
    total += read;
  }
  return contents;
}

void write_container_index_v1_file(const std::wstring_view path,
                                   const std::span<const std::byte> contents) {
  require_whole_file(contents.size());

  const std::wstring target(path);
  StagingFile staging(staging_path_for(target));
  write_new_file(staging.path(), contents);
  publish_replacement(staging.path(), target);
  staging.release();
}

} // namespace usb_vault::application
