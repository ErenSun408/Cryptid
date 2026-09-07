#include "usb_vault/application/product_settings_v1.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace usb_vault::application {
namespace {

constexpr std::wstring_view kProductDirectory = L"usb-vault";
constexpr std::wstring_view kSettingsFileName = L"settings.txt";
constexpr std::string_view kRememberKey = "remember-container-paths=";
constexpr std::string_view kMountImportedKey = "mount-imported-containers=";
constexpr std::size_t kMaximumSettingsSize = 4096;

// Reads one key. A key that is not there, or has nothing after it, is not a
// choice, so it answers the same as no file at all.
[[nodiscard]] bool value_of(const std::string &content,
                            const std::string_view key) {
  const std::size_t at = content.find(key);
  if (at == std::string::npos) return false;
  const std::size_t value = at + key.size();
  if (value >= content.size()) return false;
  return content[value] == '1';
}

// The whole file, always written as a whole. Both keys are always present, so
// a file written by this product never depends on a default to be read back.
[[nodiscard]] std::string settings_text(const bool remember,
                                        const bool mount_imported) {
  std::string text = "# usb-vault settings\r\n";
  text += kRememberKey;
  text += remember ? '1' : '0';
  text += "\r\n";
  text += kMountImportedKey;
  text += mount_imported ? '1' : '0';
  text += "\r\n";
  return text;
}

[[noreturn]] void throw_last_error(const DWORD error,
                                   const char *const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
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

[[nodiscard]] std::wstring parent_directory(const std::wstring_view path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring_view::npos) {
    return {};
  }
  return std::wstring(path.substr(0, separator));
}

void create_parent_directory(const std::wstring_view path) {
  const std::wstring parent = parent_directory(path);
  if (parent.empty() || parent.back() == L':') {
    return;
  }
  if (::CreateDirectoryW(parent.c_str(), nullptr) != FALSE) {
    return;
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_ALREADY_EXISTS) {
    throw_last_error(error, "create the product settings directory");
  }
}

// Reads the whole file, or nothing at all. A file too large to be settings is
// treated as unreadable rather than parsed: whatever it is, it is not this.
[[nodiscard]] std::optional<std::string>
read_settings(const std::wstring &path) {
  const FileHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    return std::nullopt;
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart <= 0 ||
      size.QuadPart > static_cast<LONGLONG>(kMaximumSettingsSize)) {
    return std::nullopt;
  }

  std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t total = 0;
  while (total < content.size()) {
    DWORD read = 0;
    if (::ReadFile(file.get(), content.data() + total,
                   static_cast<DWORD>(content.size() - total), &read,
                   nullptr) == FALSE) {
      return std::nullopt;
    }
    if (read == 0) return std::nullopt;
    total += read;
  }
  return content;
}

[[nodiscard]] std::wstring staging_path_for(const std::wstring_view path) {
  std::wstring result(path);
  result += L".tmp";
  return result;
}

void write_settings(const std::wstring &path, const std::string_view content) {
  create_parent_directory(path);
  const std::wstring staging = staging_path_for(path);
  {
    const FileHandle file(::CreateFileW(staging.c_str(), GENERIC_WRITE, 0,
                                        nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
      throw_last_error(::GetLastError(),
                       "create the product settings staging file");
    }
    std::size_t total = 0;
    while (total < content.size()) {
      DWORD written = 0;
      if (::WriteFile(file.get(), content.data() + total,
                      static_cast<DWORD>(content.size() - total), &written,
                      nullptr) == FALSE) {
        throw_last_error(::GetLastError(), "write the product settings");
      }
      if (written == 0) {
        throw std::runtime_error("the product settings file stopped accepting "
                                 "bytes");
      }
      total += written;
    }
    if (::FlushFileBuffers(file.get()) == FALSE) {
      throw_last_error(::GetLastError(), "flush the product settings");
    }
  }

  // Replacing in one step, so a reader never meets a half-written choice. A
  // torn settings file would read as damage, and damage here means the product
  // stops writing paths down until somebody notices.
  if (::MoveFileExW(staging.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    const DWORD error = ::GetLastError();
    static_cast<void>(::DeleteFileW(staging.c_str()));
    throw_last_error(error, "publish the product settings");
  }
}

} // namespace

std::wstring product_settings_v1_default_path() {
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
  path += kSettingsFileName;
  return path;
}

bool remember_container_paths_v1(const std::wstring_view path) {
  const std::optional<std::string> content = read_settings(std::wstring(path));
  if (!content.has_value()) {
    // Nothing has been kept here. Either this is a first run or the file is
    // unreadable, and only the first of those may answer with the default.
    const DWORD attributes = ::GetFileAttributesW(std::wstring(path).c_str());
    return attributes == INVALID_FILE_ATTRIBUTES;
  }
  return value_of(*content, kRememberKey);
}

bool mount_imported_containers_v1(const std::wstring_view path) {
  // No file, an unreadable file and a file from before this setting existed
  // all answer the same, because the product default is already the answer
  // that mounts nothing.
  const std::optional<std::string> content = read_settings(std::wstring(path));
  if (!content.has_value()) return false;
  return value_of(*content, kMountImportedKey);
}

void set_remember_container_paths_v1(const std::wstring_view path,
                                     const bool remember) {
  write_settings(std::wstring(path),
                 settings_text(remember, mount_imported_containers_v1(path)));
}

void set_mount_imported_containers_v1(const std::wstring_view path,
                                      const bool mount) {
  write_settings(std::wstring(path),
                 settings_text(remember_container_paths_v1(path), mount));
}

}  // namespace usb_vault::application
