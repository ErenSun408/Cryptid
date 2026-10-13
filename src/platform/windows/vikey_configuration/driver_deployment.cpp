#include <Windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <array>
#include "driver_deployment.h"
#include "installers.h"
#include "usb_vault/volume/volume_provider.h"
#include <string>
#include <vector>
#include <memory>
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
void phase(Diagnostic& diagnostic, Step step) { diagnostic = {step}; }
setup::Result failed(Diagnostic& diagnostic, DWORD code,
                     setup::Result result = setup::Result::unavailable) {
  diagnostic.kind = CodeKind::win32;
  diagnostic.code = code;
  return result;
}
setup::Result recheck(Diagnostic& diagnostic) {
  const auto result = probe(&diagnostic);
  if (result == setup::Result::ready) return result;
  if (diagnostic.step == Step::none)
    diagnostic = {Step::post_install_probe, CodeKind::none, 0, static_cast<std::uint32_t>(result)};
  return setup::Result::failed;
}
// Do not CopyFile from a user-writable package into System32: on modern
// Windows it can copy source security properties as well as bytes.
bool copy_new_driver(const std::filesystem::path& source, const std::filesystem::path& target, Diagnostic& diagnostic) {
  phase(diagnostic, Step::target_security);
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GR;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) {
    failed(diagnostic, GetLastError()); return false;
  }
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  phase(diagnostic, Step::target_create);
  const auto output = CreateFileW(target.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
      &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const auto output_error = output == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  LocalFree(descriptor);
  if (output == INVALID_HANDLE_VALUE) { failed(diagnostic, output_error); return false; }
  phase(diagnostic, Step::source_open);
  const auto input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  bool ok = input != INVALID_HANDLE_VALUE;
  if (!ok) failed(diagnostic, GetLastError());
  std::array<BYTE, 65536> buffer{};
  while (ok) {
    DWORD read{}, written{};
    phase(diagnostic, Step::copy_read);
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      failed(diagnostic, GetLastError()); ok = false; break;
    }
    if (!read) break;
    phase(diagnostic, Step::copy_write);
    ok = WriteFile(output, buffer.data(), read, &written, nullptr) != FALSE;
    if (!ok) failed(diagnostic, GetLastError());
    else if (written != read) { failed(diagnostic, ERROR_WRITE_FAULT); ok = false; }
  }
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  if (ok) {
    phase(diagnostic, Step::copy_flush);
    ok = FlushFileBuffers(output) != FALSE;
    if (!ok) failed(diagnostic, GetLastError());
  }
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
  if (!size || size >= MAX_PATH)
    throw std::system_error(static_cast<int>(size ? ERROR_INSUFFICIENT_BUFFER : GetLastError()),
        std::system_category(), "system directory unavailable");
  return std::filesystem::path(path) / L"drivers" / L"veracrypt.sys";
}
setup::Result probe(Diagnostic* output) noexcept {
  Diagnostic local{};
  Diagnostic& diagnostic = output ? *output : local;
  phase(diagnostic, Step::system_path);
  try {
    const auto path = system_driver_path();
    phase(diagnostic, Step::service_manager);
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return failed(diagnostic, GetLastError());
    phase(diagnostic, Step::service_open);
    Service service{OpenServiceW(manager.value, L"veracrypt", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
    if (!service.value) {
      const auto service_error = GetLastError();
      if (service_error != ERROR_SERVICE_DOES_NOT_EXIST) return failed(diagnostic, service_error);
      phase(diagnostic, Step::driver_presence);
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES) return setup::Result::unavailable;
      const auto file_error = GetLastError();
      if (file_error != ERROR_FILE_NOT_FOUND) return failed(diagnostic, file_error);
      // An unregistered portable driver can still own the device name.
      phase(diagnostic, Step::device_open);
      const auto device = CreateFileW(L"\\\\.\\VeraCrypt", 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, 0, nullptr);
      if (device != INVALID_HANDLE_VALUE) { CloseHandle(device); return setup::Result::unavailable; }
      const auto device_error = GetLastError();
      if (device_error != ERROR_FILE_NOT_FOUND && device_error != ERROR_PATH_NOT_FOUND)
        return failed(diagnostic, device_error);
      diagnostic = {};
      return classify(Presence::absent, false);
    }
    phase(diagnostic, Step::service_configuration);
    DWORD size = 0;
    QueryServiceConfigW(service.value, nullptr, 0, &size);
    const auto config_error = GetLastError();
    if (config_error != ERROR_INSUFFICIENT_BUFFER) return failed(diagnostic, config_error);
    if (size == 0 || size > 65536) return setup::Result::unavailable;
    std::vector<unsigned char> buffer(size);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service.value, config, size, &size)) return failed(diagnostic, GetLastError());
    // BOOT_START/system-encryption and nonstandard service paths require manual
    // handling. Do not rewrite their configuration to match our data-volume use.
    if (config->dwServiceType != SERVICE_KERNEL_DRIVER ||
        config->dwStartType != SERVICE_SYSTEM_START ||
        !(equal(config->lpBinaryPathName, L"System32\\drivers\\veracrypt.sys") ||
          equal(config->lpBinaryPathName, path.native()) ||
          equal(config->lpBinaryPathName, L"\\SystemRoot\\System32\\drivers\\veracrypt.sys")))
      return classify(Presence::incompatible, false);
    phase(diagnostic, Step::existing_verification);
    auto trusted = verify_veracrypt_driver(path);
    phase(diagnostic, Step::service_status);
    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
          reinterpret_cast<BYTE*>(&status), sizeof(status), &size)) return failed(diagnostic, GetLastError());
    if (status.dwCurrentState == SERVICE_STOPPED) {
      diagnostic.detail = status.dwCurrentState;
      return classify(Presence::trusted_stopped, false);
    }
    if (status.dwCurrentState != SERVICE_RUNNING) {
      diagnostic.detail = status.dwCurrentState;
      return classify(Presence::existing_unavailable, false);
    }
    phase(diagnostic, Step::driver_version);
    const auto version = usb_vault::volume::VolumeProvider{}.driver_version();
    if (version != 0x126) { diagnostic.detail = version; return classify(Presence::incompatible, false); }
    diagnostic = {};
    return classify(Presence::trusted_running, false);
  } catch (const std::system_error& error) {
    capture_exception(diagnostic, error); return setup::Result::unavailable;
  } catch (...) { return setup::Result::unavailable; }
}
setup::Result install_elevated(const std::filesystem::path& directory, Diagnostic* output) noexcept {
  Diagnostic local{};
  Diagnostic& diagnostic = output ? *output : local;
  phase(diagnostic, Step::elevation_token);
  try {
    // Authority is established by the OS token, never by a command-line flag.
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return failed(diagnostic, GetLastError());
    TOKEN_ELEVATION elevation{};
    DWORD count{};
    const bool inspected = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &count) != FALSE;
    const auto token_error = inspected ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!inspected) return failed(diagnostic, token_error);
    if (!elevation.TokenIsElevated) return failed(diagnostic, ERROR_ELEVATION_REQUIRED);
    const auto before = probe(&diagnostic);
    if (before != setup::Result::missing && before != setup::Result::repairable) return before;
    phase(diagnostic, Step::source_verification);
    auto source = verify_veracrypt_driver(directory / L"prerequisites" / L"veracrypt-x64.sys");
    phase(diagnostic, Step::system_path);
    const auto target = system_driver_path();
    phase(diagnostic, Step::target_directory);
    const auto folder = CreateFileW(target.parent_path().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (folder == INVALID_HANDLE_VALUE) return failed(diagnostic, GetLastError());
    struct FolderGuard { HANDLE value; ~FolderGuard() { CloseHandle(value); } } folder_guard{folder};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(folder, FileAttributeTagInfo, &attributes, sizeof(attributes)))
      return failed(diagnostic, GetLastError());
    if (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return setup::Result::unavailable;
    phase(diagnostic, Step::service_manager);
    Service manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) return failed(diagnostic, GetLastError());
    if (before == setup::Result::repairable) {
      // Explicit repair only starts a verified stopped service. Never delete,
      // reconfigure or replace it, even if loading fails.
      phase(diagnostic, Step::existing_verification);
      auto installed = verify_veracrypt_driver(target);
      const auto now = probe(&diagnostic);
      if (now == setup::Result::ready) return now;
      if (now != setup::Result::repairable) return setup::Result::unavailable;
      phase(diagnostic, Step::service_open);
      Service service{OpenServiceW(manager.value, L"veracrypt", SERVICE_START)};
      if (!service.value) return failed(diagnostic, GetLastError(), setup::Result::failed);
      phase(diagnostic, Step::service_start);
      if (!StartServiceW(service.value, 0, nullptr)) return failed(diagnostic, GetLastError(), setup::Result::failed);
      return recheck(diagnostic);
    }
    if (!copy_new_driver(source.path(), target, diagnostic)) return setup::Result::failed;
    try {
      phase(diagnostic, Step::target_verification);
      auto installed = verify_veracrypt_driver(target);
      phase(diagnostic, Step::service_create);
      Service service{CreateServiceW(manager.value, L"veracrypt", L"veracrypt",
          SERVICE_START | SERVICE_QUERY_STATUS | DELETE, SERVICE_KERNEL_DRIVER,
          SERVICE_SYSTEM_START, SERVICE_ERROR_NORMAL, L"System32\\drivers\\veracrypt.sys",
          nullptr, nullptr, nullptr, nullptr, nullptr)};
      if (!service.value) {
        // A racing service may now use this file. Never remove it on this path.
        return failed(diagnostic, GetLastError());
      }
      phase(diagnostic, Step::service_start);
      if (!StartServiceW(service.value, 0, nullptr)) {
        // Retain both the signed file and its service for explicit repair.
        // Removing only the service would turn a recoverable stopped driver
        // into an orphan that must correctly fail closed on the next attempt.
        return failed(diagnostic, GetLastError(), setup::Result::failed);
      }
      // Kernel service start is synchronous; the device must be queryable now.
      return recheck(diagnostic);
    } catch (const std::system_error& error) {
      capture_exception(diagnostic, error); return setup::Result::failed;
    } catch (...) {
      // No service was created by us on this exception path. A signed orphan
      // is safer than deleting a file another administrator may have adopted.
      return setup::Result::failed;
    }
  } catch (const std::system_error& error) {
    capture_exception(diagnostic, error); return setup::Result::failed;
  } catch (...) { return setup::Result::failed; }
}
setup::Result request_install(const std::filesystem::path& executable, Diagnostic* output) noexcept {
  Diagnostic local{};
  Diagnostic& diagnostic = output ? *output : local;
  diagnostic = {};
  try {
    const auto before = probe(&diagnostic);
    if (before != setup::Result::missing && before != setup::Result::repairable) return before;
    // Pin the payload throughout UAC; the elevated child verifies it again.
    phase(diagnostic, Step::source_verification);
    auto source = verify_veracrypt_driver(executable.parent_path() / L"prerequisites" / L"veracrypt-x64.sys");
    std::unique_ptr<DiagnosticReceiver> receiver;
    try { receiver = std::make_unique<DiagnosticReceiver>(); }
    catch (...) { /* Reporting is optional; never block installation on IPC availability. */ }
    const std::wstring parameters = receiver ?
        L"--install-driver-elevated --diagnostic-pipe " + receiver->token() : L"--install-driver-elevated";
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"runas";
    launch.lpFile = executable.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_HIDE;
    phase(diagnostic, Step::launch_elevated);
    if (!ShellExecuteExW(&launch)) {
      const auto error = GetLastError();
      return failed(diagnostic, error, error == ERROR_CANCELLED ? setup::Result::cancelled : setup::Result::failed);
    }
    if (!launch.hProcess) return setup::Result::failed;
    struct Child { HANDLE value; ~Child() { CloseHandle(value); } } child{launch.hProcess};
    phase(diagnostic, Step::wait_elevated);
    const auto child_pid = GetProcessId(launch.hProcess);
    const auto wait = WaitForSingleObject(launch.hProcess, INFINITE);
    DWORD code{};
    const bool finished = wait == WAIT_OBJECT_0 && GetExitCodeProcess(launch.hProcess, &code);
    const auto wait_error = finished ? ERROR_SUCCESS : GetLastError();
    if (!finished) return failed(diagnostic, wait_error, setup::Result::failed);
    if (code == static_cast<DWORD>(setup::Result::ready)) return probe(&diagnostic);
    // A missing, malformed or foreign report cannot turn failure into success.
    diagnostic = {Step::wait_elevated, CodeKind::process_exit, code};
    if (receiver) static_cast<void>(receiver->receive(child_pid, code, diagnostic));
    if (code == static_cast<DWORD>(setup::Result::cancelled)) return setup::Result::cancelled;
    if (code == static_cast<DWORD>(setup::Result::incompatible)) return setup::Result::incompatible;
    if (code == static_cast<DWORD>(setup::Result::unavailable)) return setup::Result::unavailable;
    return setup::Result::failed;
  } catch (const std::system_error& error) {
    capture_exception(diagnostic, error); return setup::Result::failed;
  } catch (...) { return setup::Result::failed; }
}
}
