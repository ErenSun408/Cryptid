#include "volume/volume_containment.h"

#include <Windows.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace usb_vault::volume {
namespace {

[[noreturn]] void throw_last_error(const DWORD error,
                                   const char* const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

class Handle final {
 public:
  explicit Handle(const HANDLE handle) noexcept : handle_(handle) {}
  ~Handle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&&) = delete;
  Handle& operator=(Handle&&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

}  // namespace

wchar_t upper_ascii(const wchar_t character) noexcept {
  return (character >= L'a' && character <= L'z')
             ? static_cast<wchar_t>(character - L'a' + L'A')
             : character;
}

wchar_t require_drive_absolute(const std::wstring_view path) {
  if (path.size() < 3 || path[1] != L':' ||
      (path[2] != L'\\' && path[2] != L'/') || upper_ascii(path[0]) < L'A' ||
      upper_ascii(path[0]) > L'Z') {
    throw std::invalid_argument("path is not drive-absolute");
  }
  return path[0];
}

void require_directory_stays_on_volume(const std::wstring& directory,
                                       const wchar_t drive) {
  // Opened without `FILE_FLAG_OPEN_REPARSE_POINT` on purpose. The point is not
  // to refuse to open a link, it is to find out where following one arrives.
  const Handle handle(::CreateFileW(
      directory.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    throw_last_error(::GetLastError(), "open the directory");
  }

  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle.get(), &information) == FALSE) {
    throw_last_error(::GetLastError(), "inspect the directory");
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    throw std::runtime_error("the path is not a directory");
  }

  const DWORD length =
      ::GetFinalPathNameByHandleW(handle.get(), nullptr, 0, VOLUME_NAME_DOS);
  if (length == 0) throw_last_error(::GetLastError(), "resolve the directory");
  std::wstring final_path(length, L'\0');
  const DWORD written = ::GetFinalPathNameByHandleW(
      handle.get(), final_path.data(), length, VOLUME_NAME_DOS);
  if (written == 0 || written >= length) {
    throw_last_error(::GetLastError(), "resolve the directory");
  }
  final_path.resize(written);

  // `VOLUME_NAME_DOS` answers as `\\?\X:\...`, and for a volume reachable only
  // through a mount point it answers as `\\?\Volume{...}\...`. The second form
  // is not this product's shape -- containers mount on drive letters -- and it
  // is refused rather than parsed, because a directory that cannot name its
  // own drive cannot prove it stayed on it.
  constexpr std::wstring_view kPrefix = L"\\\\?\\";
  if (!final_path.starts_with(kPrefix)) {
    throw std::runtime_error("the directory has no drive-letter path");
  }
  const std::wstring_view resolved =
      std::wstring_view(final_path).substr(kPrefix.size());
  if (resolved.size() < 2 || resolved[1] != L':' ||
      upper_ascii(resolved[0]) != upper_ascii(drive)) {
    throw std::runtime_error("the directory resolves outside its volume");
  }
}

void require_parent_stays_on_volume(const std::wstring& path,
                                    const wchar_t drive) {
  const std::size_t separator = path.find_last_of(L"\\/");
  // A drive-absolute path always holds one at index 2, so this cannot be npos
  // for anything `require_drive_absolute` accepted.
  if (separator == std::wstring::npos) {
    throw std::invalid_argument("path has no parent directory");
  }
  // Keep the separator for a root, so `X:\a` asks about `X:\` and not `X:`,
  // which names the process's current directory on that drive instead.
  const std::wstring parent =
      separator == 2 ? path.substr(0, 3) : path.substr(0, separator);
  require_directory_stays_on_volume(parent, drive);
}

}  // namespace usb_vault::volume
