#include "usb_vault/application/container_file_visibility.h"
#include <Windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
namespace usb_vault::application {
namespace {
struct FileHandle {
  HANDLE value;
  ~FileHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};
[[noreturn]] void fail() { throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "file visibility failed"); }
void validate_path(std::wstring_view path) {
  if (path.empty() || path.find(L'\0') != path.npos || !std::filesystem::path(path).is_absolute() ||
      path.starts_with(L"\\\\.\\") || path.starts_with(L"\\\\?\\") || path.find(L':', 2) != path.npos)
    throw std::invalid_argument("invalid container file path");
}
HANDLE open_file(std::wstring_view path, DWORD access, DWORD sharing) {
  validate_path(path);
  const std::wstring owned(path);
  FileHandle file{::CreateFileW(owned.c_str(), access, sharing,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE) fail();
  BY_HANDLE_FILE_INFORMATION identity{};
  if (!::GetFileInformationByHandle(file.value, &identity)) fail();
  if ((identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || identity.nNumberOfLinks != 1)
    throw std::invalid_argument("container must be a single-link regular file");
  return std::exchange(file.value, INVALID_HANDLE_VALUE);
}
}
bool container_file_hidden(std::wstring_view path) {
  FileHandle file{open_file(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)};
  FILE_BASIC_INFO info{};
  if (!::GetFileInformationByHandleEx(file.value, FileBasicInfo, &info, sizeof(info))) fail();
  constexpr DWORD mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
  return (info.FileAttributes & mask) == mask;
}
void set_container_file_visibility(std::wstring_view path, bool hidden) {
  FileHandle file{open_file(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ)};
  FILE_BASIC_INFO before{};
  if (!::GetFileInformationByHandleEx(file.value, FileBasicInfo, &before, sizeof(before))) fail();
  constexpr DWORD mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
  FILE_BASIC_INFO change{}; // Zero timestamps leave existing times untouched.
  change.FileAttributes = hidden ? (before.FileAttributes & ~FILE_ATTRIBUTE_NORMAL) | mask : before.FileAttributes & ~mask;
  if (!change.FileAttributes) change.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  if (change.FileAttributes == before.FileAttributes) return;
  if (!::SetFileInformationByHandle(file.value, FileBasicInfo, &change, sizeof(change))) fail();
  FILE_BASIC_INFO after{};
  if (!::GetFileInformationByHandleEx(file.value, FileBasicInfo, &after, sizeof(after))) fail();
  if (after.FileAttributes != change.FileAttributes) throw std::runtime_error("file visibility verification failed");
}
void create_protected_directory(std::wstring_view path) {
  validate_path(path);
  const std::wstring owned(path);
  if (!::CreateDirectoryW(owned.c_str(), nullptr)) fail();
  try {
    FileHandle directory{::CreateFileW(owned.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (directory.value == INVALID_HANDLE_VALUE) fail();
    FILE_BASIC_INFO before{};
    if (!::GetFileInformationByHandleEx(directory.value, FileBasicInfo, &before, sizeof(before))) fail();
    if ((before.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY)
      throw std::runtime_error("protected directory is not a plain directory");
    constexpr DWORD mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    FILE_BASIC_INFO change{}; // Zero timestamps leave existing times untouched.
    change.FileAttributes = (before.FileAttributes & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL)) | mask;
    if (!::SetFileInformationByHandle(directory.value, FileBasicInfo, &change, sizeof(change))) fail();
    FILE_BASIC_INFO after{};
    if (!::GetFileInformationByHandleEx(directory.value, FileBasicInfo, &after, sizeof(after))) fail();
    if ((after.FileAttributes & mask) != mask) throw std::runtime_error("protected directory verification failed");
  } catch (...) {
    // The handle is closed by now; the folder is new and empty, so it can go.
    ::RemoveDirectoryW(owned.c_str());
    throw;
  }
}
void remove_empty_directory(std::wstring_view path) {
  validate_path(path);
  const std::wstring owned(path);
  if (!::RemoveDirectoryW(owned.c_str())) fail();
}
}
