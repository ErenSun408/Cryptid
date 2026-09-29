#include <Windows.h>
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
    if (status.dwCurrentState != SERVICE_RUNNING)
      return classify(Presence::existing_unavailable, false);
    return usb_vault::volume::VolumeProvider{}.driver_version() == 0x126
        ? classify(Presence::trusted_running, false) : classify(Presence::incompatible, false);
  } catch (...) { return setup::Result::unavailable; }
}
}
