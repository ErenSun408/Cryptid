#include <Windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <array>
#include "driver_deployment.h"
#include "installers.h"
#include "usb_vault/volume/volume_provider.h"
#include <string>
#include <vector>
#include <stdexcept>

namespace configuration::driver {
namespace {
struct Service {
  SC_HANDLE value{};
  ~Service() { if (value) CloseServiceHandle(value); }
};
bool equal(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
// Do not CopyFile from a user-writable package into System32: on modern
// Windows it can copy source security properties as well as bytes.
bool copy_new_driver(const std::filesystem::path& source, const std::filesystem::path& target) {
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  const auto output = CreateFileW(target.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
      &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  LocalFree(descriptor);
  if (output == INVALID_HANDLE_VALUE) return false;
  const auto input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  bool ok = input != INVALID_HANDLE_VALUE;
  std::array<BYTE, 65536> buffer{};
  while (ok) {
    DWORD read{}, written{};
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    ok = WriteFile(output, buffer.data(), read, &written, nullptr) && written == read;
  }
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  if (ok) ok = FlushFileBuffers(output) != FALSE;
  if (!ok) {
    // This is our still-open CREATE_NEW file, before any service was created.
    FILE_DISPOSITION_INFO remove{TRUE};
    SetFileInformationByHandle(output, FileDispositionInfo, &remove, sizeof(remove));
  }
  CloseHandle(output);
  return ok;
}
}
std::filesystem::path system_driver_path() {
  wchar_t path[MAX_PATH]{};
  const auto size = GetSystemDirectoryW(path, MAX_PATH);
  if (!size || size >= MAX_PATH) throw std::runtime_error("system directory unavailable");
  return std::filesystem::path(path) / L"drivers" / L"veracrypt.sys";
}
setup::Result probe() noexcept {
  try {
    const auto path = system_driver_path();
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return setup::Result::unavailable;
    Service service{OpenServiceW(manager.value, L"veracrypt", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
    if (!service.value) {
      if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) return setup::Result::unavailable;
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES) return setup::Result::unavailable;
      if (GetLastError() != ERROR_FILE_NOT_FOUND) return setup::Result::unavailable;
      // An unregistered portable driver can still own the device name.
      const auto device = CreateFileW(L"\\\\.\\VeraCrypt", 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, 0, nullptr);
      if (device != INVALID_HANDLE_VALUE) { CloseHandle(device); return setup::Result::unavailable; }
      if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
        return setup::Result::unavailable;
      return classify(Presence::absent, false);
    }
    DWORD size = 0;
    QueryServiceConfigW(service.value, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 65536)
      return setup::Result::unavailable;
    std::vector<unsigned char> buffer(size);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service.value, config, size, &size)) return setup::Result::unavailable;
    // BOOT_START/system-encryption and nonstandard service paths require manual
    // handling. Do not rewrite their configuration to match our data-volume use.
    if (config->dwServiceType != SERVICE_KERNEL_DRIVER ||
        config->dwStartType != SERVICE_SYSTEM_START ||
        !(equal(config->lpBinaryPathName, L"System32\\drivers\\veracrypt.sys") ||
          equal(config->lpBinaryPathName, path.native()) ||
          equal(config->lpBinaryPathName, L"\\SystemRoot\\System32\\drivers\\veracrypt.sys")))
      return classify(Presence::incompatible, false);
    auto trusted = verify_veracrypt_driver(path);
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
          reinterpret_cast<BYTE*>(&status), sizeof(status), &size)) return setup::Result::unavailable;
    if (status.dwCurrentState == SERVICE_STOPPED)
      return classify(Presence::trusted_stopped, false);
    if (status.dwCurrentState != SERVICE_RUNNING)
      return classify(Presence::existing_unavailable, false);
    return usb_vault::volume::VolumeProvider{}.driver_version() == 0x126
        ? classify(Presence::trusted_running, false) : classify(Presence::incompatible, false);
  } catch (...) { return setup::Result::unavailable; }
}
setup::Result install_elevated(const std::filesystem::path& directory) noexcept {
  try {
    // Authority is established by the OS token, never by a command-line flag.
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return setup::Result::unavailable;
    TOKEN_ELEVATION elevation{};
    DWORD count{};
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &count)
        && elevation.TokenIsElevated;
    CloseHandle(token);
    if (!elevated) return setup::Result::unavailable;
    const auto before = probe();
    if (before != setup::Result::missing && before != setup::Result::repairable) return before;
    auto source = verify_veracrypt_driver(directory / L"prerequisites" / L"veracrypt-x64.sys");
    const auto target = system_driver_path();
    const auto folder = CreateFileW(target.parent_path().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (folder == INVALID_HANDLE_VALUE) return setup::Result::unavailable;
    struct FolderGuard { HANDLE value; ~FolderGuard() { CloseHandle(value); } } folder_guard{folder};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(folder, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return setup::Result::unavailable;
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) return setup::Result::unavailable;
    if (before == setup::Result::repairable) {
      // Explicit repair only starts a verified stopped service. Never delete,
      // reconfigure or replace it, even if loading fails.
      auto installed = verify_veracrypt_driver(target);
      const auto now = probe();
      if (now == setup::Result::ready) return now;
      if (now != setup::Result::repairable) return setup::Result::unavailable;
      Service service{OpenServiceW(manager.value, L"veracrypt", SERVICE_START)};
      if (!service.value || !StartServiceW(service.value, 0, nullptr)) return setup::Result::failed;
      return probe() == setup::Result::ready ? setup::Result::ready : setup::Result::failed;
    }
    if (!copy_new_driver(source.path(), target)) return setup::Result::failed;
    try {
      auto installed = verify_veracrypt_driver(target);
      Service service{CreateServiceW(manager.value, L"veracrypt", L"veracrypt",
          SERVICE_START | SERVICE_QUERY_STATUS | DELETE, SERVICE_KERNEL_DRIVER,
          SERVICE_SYSTEM_START, SERVICE_ERROR_NORMAL, L"System32\\drivers\\veracrypt.sys",
          nullptr, nullptr, nullptr, nullptr, nullptr)};
      if (!service.value) {
        // A racing service may now use this file. Never remove it on this path.
        return setup::Result::unavailable;
      }
      if (!StartServiceW(service.value, 0, nullptr)) {
        // Retain both the signed file and its service for explicit repair.
        // Removing only the service would turn a recoverable stopped driver
        // into an orphan that must correctly fail closed on the next attempt.
        return setup::Result::failed;
      }
      // Kernel service start is synchronous; the device must be queryable now.
      return probe() == setup::Result::ready ? setup::Result::ready : setup::Result::failed;
    } catch (...) {
      // No service was created by us on this exception path. A signed orphan
      // is safer than deleting a file another administrator may have adopted.
      return setup::Result::failed;
    }
  } catch (...) { return setup::Result::failed; }
}
setup::Result request_install(const std::filesystem::path& executable) noexcept {
  try {
    const auto before = probe();
    if (before != setup::Result::missing && before != setup::Result::repairable) return before;
    // Pin the payload throughout UAC; the elevated child verifies it again.
    auto source = verify_veracrypt_driver(executable.parent_path() / L"prerequisites" / L"veracrypt-x64.sys");
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = L"--install-driver-elevated";
    launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch))
      return GetLastError() == ERROR_CANCELLED ? setup::Result::cancelled : setup::Result::failed;
    if (!launch.hProcess) return setup::Result::failed;
    const auto wait = WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD code{};
    const bool finished = wait == WAIT_OBJECT_0 && GetExitCodeProcess(launch.hProcess, &code);
    CloseHandle(launch.hProcess);
    if (!finished) return setup::Result::failed;
    if (code == static_cast<DWORD>(setup::Result::ready)) return probe();
    if (code == static_cast<DWORD>(setup::Result::cancelled)) return setup::Result::cancelled;
    return setup::Result::failed;
  } catch (...) { return setup::Result::failed; }
}
}
