#include "driver_cleanup.h"
#include "driver_deployment.h"
#include "installers.h"
#include <Windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace configuration::driver_cleanup {
namespace {
using setup::Result;
void require(bool value) { if (!value) throw std::runtime_error("driver cleanup unavailable"); }
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE v) : value(v) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  Handle(const Handle&) = delete;
};
struct Service {
  SC_HANDLE value{};
  ~Service() { close(); }
  void close() { if (value) CloseServiceHandle(std::exchange(value, nullptr)); }
};
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
bool equal(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
Handle pin(const std::filesystem::path& path, bool directory) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  require(file.value != INVALID_HANDLE_VALUE);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  require(GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) &&
      !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory);
  std::wstring final(32768, L'\0');
  const auto size = GetFinalPathNameByHandleW(file.value, final.data(), static_cast<DWORD>(final.size()), FILE_NAME_NORMALIZED);
  require(size && size < final.size()); final.resize(size);
  if (final.starts_with(L"\\\\?\\")) final.erase(0, 4);
  require(equal(final, path.native()));
  return file;
}
std::vector<Handle> pin_ancestors(const std::filesystem::path& path) {
  require(path.is_absolute() && path.native().size() < 240 && GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED);
  std::vector<Handle> handles;
  auto current = path.root_path(); handles.push_back(pin(current, true));
  for (const auto& part : path.relative_path()) { current /= part; handles.push_back(pin(current, true)); }
  return handles;
}
bool device_absent() {
  Handle device(CreateFileW(L"\\\\.\\VeraCrypt", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  return device.value == INVALID_HANDLE_VALUE && missing(GetLastError());
}
constexpr DWORD ioctl(DWORD code) { return CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800U + code, METHOD_BUFFERED, FILE_ANY_ACCESS); }
bool query_integer(HANDLE device, DWORD code, LONG& value) {
  DWORD size{};
  return DeviceIoControl(device, ioctl(code), nullptr, 0, &value, sizeof(value), &size, nullptr) && size == sizeof(value);
}
// Clean-room wire layout for the pinned 1.26 ABI; no VeraCrypt headers linked.
#pragma pack(push, 1)
struct BootStatus {
  LONG filter; WORD loader;
  LONG mounted, header, encrypted;
  std::array<LONGLONG, 5> areas;
  DWORD salt; LONG setup, mode, waiting; DWORD hibernation;
  LONG hidden; LONGLONG partition; DWORD leaks; LONG vulnerable;
};
#pragma pack(pop)
static_assert(sizeof(BootStatus) == 98);
bool driver_idle() {
  Handle device(CreateFileW(L"\\\\.\\VeraCrypt", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
  if (device.value == INVALID_HANDLE_VALUE) return false;
  LONG version{}, mounted{}, references{};
  BootStatus boot{}; DWORD size{};
  return query_integer(device.value, 1, version) && version == 0x126 &&
      query_integer(device.value, 10, mounted) && mounted == 0 &&
      query_integer(device.value, 8, references) && references == 1 &&
      DeviceIoControl(device.value, ioctl(18), nullptr, 0, &boot, sizeof(boot), &size, nullptr) && size == sizeof(boot) &&
      boot.filter == 0 && boot.mounted == 0 && boot.header == 0 && boot.encrypted == 0 &&
      boot.setup == 0 && boot.hidden == 0;
}
bool elevated() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
  Handle token(raw); TOKEN_ELEVATION elevation{}; DWORD size{};
  return GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
}
class Operations {
 public:
  explicit Operations(bool writable) : target_(driver::system_driver_path()), ancestors_(pin_ancestors(target_.parent_path())) {
    manager_.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    require(manager_.value != nullptr);
    service_.value = OpenServiceW(manager_.value, L"veracrypt", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
        (writable ? SERVICE_STOP | DELETE : 0));
    if (!service_.value) require(GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST);
  }
  State inspect() {
    if (!service_.value) {
      if (!device_absent()) return State::blocked;
      const auto attrs = GetFileAttributesW(target_.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) return missing(GetLastError()) ? State::absent : State::blocked;
      return State::orphan;
    }
    if (!standard_service()) return State::blocked;
    const auto state = status();
    return state == SERVICE_STOPPED ? State::stopped : state == SERVICE_RUNNING ? State::running : State::blocked;
  }
  bool verify() { verified_.emplace(verify_veracrypt_driver(target_)); return true; }
  bool idle() { return driver_idle(); }
  bool stop() {
    // A new client can attach after idle() closes its handle. SCM/NT must
    // accept normal unload; there is no force-unmount or kill fallback.
    if (!standard_service() || status() != SERVICE_RUNNING || !driver_idle()) return false;
    SERVICE_STATUS stopped_status{};
    if (!ControlService(service_.value, SERVICE_CONTROL_STOP, &stopped_status)) return false;
    for (int i = 0; i < 20; ++i) {
      if (status() == SERVICE_STOPPED) return device_absent();
      Sleep(100);
    }
    return false;
  }
  bool stopped() { return (!service_.value || (standard_service() && status() == SERVICE_STOPPED)) && device_absent(); }
  bool prepare_delete() {
    verified_.reset();
    verified_.emplace(verify_veracrypt_driver(target_, true));
    return stopped();
  }
  bool delete_service() {
    if (!stopped() || !DeleteService(service_.value)) return false;
    service_.close();
    for (int i = 0; i < 20; ++i) {
      if (service_absent()) return true;
      Sleep(100);
    }
    return false; // Marked-for-delete still counts as incomplete.
  }
  bool delete_file() {
    if (!service_absent() || !device_absent() || !verified_->remove_verified_file()) return false;
    verified_.reset(); return true;
  }
  bool absent() {
    const auto attrs = GetFileAttributesW(target_.c_str());
    const bool absent_file = attrs == INVALID_FILE_ATTRIBUTES && missing(GetLastError());
    return absent_file && service_absent() && device_absent();
  }
 private:
  bool service_absent() {
    Service check{OpenServiceW(manager_.value, L"veracrypt", SERVICE_QUERY_STATUS)};
    return !check.value && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
  }
  DWORD status() {
    SERVICE_STATUS_PROCESS value{}; DWORD size{};
    require(QueryServiceStatusEx(service_.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&value), sizeof(value), &size) != FALSE);
    return value.dwCurrentState;
  }
  bool standard_service() {
    DWORD size{}; QueryServiceConfigW(service_.value, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536) return false;
    std::vector<BYTE> buffer(size); auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service_.value, cfg, size, &size)) return false;
    return cfg->dwServiceType == SERVICE_KERNEL_DRIVER && cfg->dwStartType == SERVICE_SYSTEM_START &&
        (equal(cfg->lpBinaryPathName, L"System32\\drivers\\veracrypt.sys") || equal(cfg->lpBinaryPathName, target_.native()) ||
         equal(cfg->lpBinaryPathName, L"\\SystemRoot\\System32\\drivers\\veracrypt.sys"));
  }
  std::filesystem::path target_;
  std::vector<Handle> ancestors_;
  Service manager_, service_;
  std::optional<usb_vault::card::VerifiedLocalArtifact> verified_;
};
}
Result uninstall_elevated() noexcept {
  if (!elevated()) return Result::unavailable;
  try { Operations operations(true); return remove(operations); }
  catch (...) { return Result::unavailable; }
}
Result request_uninstall(const std::filesystem::path& executable) noexcept {
  try {
    {
      Operations preflight(false);
      const auto state = preflight.inspect();
      if (state == State::absent) return Result::ready;
      if (state == State::blocked || !preflight.verify() || (state == State::running && !preflight.idle())) return Result::unavailable;
    }
    auto ancestors = pin_ancestors(executable.parent_path());
    auto image = pin(executable, false);
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch); launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas"; launch.lpFile = executable.c_str();
    launch.lpParameters = L"--uninstall-driver-elevated"; launch.nShow = SW_HIDE;
    if (!ShellExecuteExW(&launch)) return GetLastError() == ERROR_CANCELLED ? Result::cancelled : Result::failed;
    Handle child(launch.hProcess);
    if (!child.value) return Result::failed;
    DWORD code{};
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(child.value, &code)) return Result::failed;
    if (code != 0) return code == static_cast<DWORD>(Result::cancelled) ? Result::cancelled : Result::failed;
    Operations after(false);
    return after.absent() ? Result::ready : Result::failed;
  } catch (...) { return Result::unavailable; }
}
}
