#include "usb_vault/application/product_settings_v1.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstddef>
#include <charconv>
#include <cstdint>
#include <limits>
#include <iterator>
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
constexpr std::string_view kPreferenceKeys[] = {"force-unmount=", "operation-logging=", "window-size=", "auto-mount=", "close-to-tray=", "container-extension=", "hide-file-containers="};
unsigned int preference_index(ProductPreference key) {
  const auto index = static_cast<unsigned int>(key);
  if (index >= std::size(kPreferenceKeys)) throw std::invalid_argument("unknown preference");
  return index;
}

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

// The whole file, always written as a whole. All keys are always present, so
// a file written by this product never depends on a default to be read back.
[[nodiscard]] std::string settings_text(const bool remember,
                                        const bool mount_imported,
                                        std::wstring_view path) {
  std::string text = "# usb-vault settings\r\n";
  text += kRememberKey;
  text += remember ? '1' : '0';
  text += "\r\n";
  text += kMountImportedKey;
  text += mount_imported ? '1' : '0';
  text += "\r\n";
  for (unsigned int i = 0; i < std::size(kPreferenceKeys); ++i) {
    text += kPreferenceKeys[i];
    text += static_cast<char>('0' + product_preference_v1(path, static_cast<ProductPreference>(i)));
    text += "\r\n";
  }
  const auto hotkey = exit_hotkey_settings_v1(path);
  text += "exit-hotkey=" + std::to_string(hotkey.enabled ? 1 : 0) + "," +
      std::to_string(hotkey.key) + "," + std::to_string(hotkey.count) + "\r\n";
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

bool valid_exit_hotkey(ExitHotkeySettings s) noexcept {
  return s.count >= 1 && s.count <= 10 &&
      ((s.key >= 0x30 && s.key <= 0x39) || (s.key >= 0x41 && s.key <= 0x5A) ||
       (s.key >= 0x70 && s.key <= 0x87) || (s.key >= 0x60 && s.key <= 0x6F) ||
       (s.key >= 0x21 && s.key <= 0x28) || (s.key >= 0xBA && s.key <= 0xC0) ||
       (s.key >= 0xDB && s.key <= 0xDF) || s.key == 0xE2 ||
       (s.key >= 0xA6 && s.key <= 0xB7) || s.key == 0x90 || s.key == 0x91 ||
       s.key == 8 || s.key == 9 || s.key == 12 || s.key == 13 || s.key == 19 ||
       s.key == 20 || s.key == 27 || s.key == 32 || s.key == 44 ||
       s.key == 45 || s.key == 46 || s.key == 93);
}

ExitHotkeySettings exit_hotkey_settings_v1(std::wstring_view path) {
  const auto content = read_settings(std::wstring(path));
  if (!content) return {};
  constexpr std::string_view prefix = "exit-hotkey=";
  ExitHotkeySettings result;
  bool found = false;
  for (std::size_t start = 0; start < content->size();) {
    const auto end = content->find('\n', start);
    auto line = std::string_view(*content).substr(start, end == std::string::npos ? content->size() - start : end - start);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.starts_with(prefix)) {
      if (found) return {};
      found = true;
      auto value = line.substr(prefix.size());
      unsigned int fields[3]{};
      for (unsigned int i = 0; i < 3; ++i) {
        const auto comma = value.find(',');
        const auto part = value.substr(0, comma);
        if (part.empty() || (part.size() > 1 && part.front() == '0')) return {};
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), fields[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
            (i < 2 ? comma == std::string_view::npos : comma != std::string_view::npos)) return {};
        if (i < 2) value.remove_prefix(comma + 1);
      }
      result = {fields[0] == 1, fields[1], fields[2]};
      if (fields[0] > 1 || !valid_exit_hotkey(result)) return {};
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

void set_exit_hotkey_settings_v1(std::wstring_view path, ExitHotkeySettings settings) {
  if (!valid_exit_hotkey(settings)) throw std::invalid_argument("invalid exit hotkey");
  auto text = settings_text(remember_container_paths_v1(path), mount_imported_containers_v1(path), path);
  const auto at = text.find("exit-hotkey=");
  text.replace(at, text.find('\n', at) - at + 1,
      "exit-hotkey=" + std::to_string(settings.enabled ? 1 : 0) + "," +
      std::to_string(settings.key) + "," + std::to_string(settings.count) + "\r\n");
  write_settings(std::wstring(path), text);
}

unsigned int product_preference_v1(std::wstring_view path, ProductPreference key) {
  const auto index = preference_index(key);
  // Retired settings are product policy, including for existing installations.
  if (key == ProductPreference::container_extension) return 3;
  if (key == ProductPreference::hide_file_containers) return 1;
  const auto fallback = index == 0 || index == 3 || index == 4 || index == 5 || index == 6 ? 0U : 1U;
  const auto content = read_settings(std::wstring(path));
  if (!content) return fallback;
  const auto prefix = kPreferenceKeys[index];
  bool found = false;
  unsigned int result = fallback;
  std::size_t start = 0;
  while (start < content->size()) {
    const auto end = content->find('\n', start);
    auto line = std::string_view(*content).substr(start, end == std::string::npos ? content->size() - start : end - start);
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.starts_with(prefix)) {
      if (found) return fallback;
      found = true;
      const auto value = line.substr(prefix.size());
      if (value.size() != 1 || value[0] < '0' || value[0] > (index == 2 || index == 5 ? '3' : '1')) return fallback;
      result = static_cast<unsigned int>(value[0] - '0');
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

void set_product_preference_v1(std::wstring_view path, ProductPreference key, unsigned int value) {
  const auto index = preference_index(key);
  if (key == ProductPreference::container_extension || key == ProductPreference::hide_file_containers) {
    const auto fixed = key == ProductPreference::container_extension ? 3U : 1U;
    if (value != fixed) throw std::invalid_argument("file creation policy is fixed");
    return;
  }
  if (value > (index == 2 || index == 5 ? 3U : 1U)) throw std::invalid_argument("invalid preference value");
  auto text = settings_text(remember_container_paths_v1(path), mount_imported_containers_v1(path), path);
  const auto at = text.find(kPreferenceKeys[index]);
  text[at + kPreferenceKeys[index].size()] = static_cast<char>('0' + value);
  write_settings(std::wstring(path), text);
}

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
                 settings_text(remember, mount_imported_containers_v1(path), path));
}

void set_mount_imported_containers_v1(const std::wstring_view path,
                                      const bool mount) {
  write_settings(std::wstring(path),
                 settings_text(remember_container_paths_v1(path), mount, path));
}

}  // namespace usb_vault::application
