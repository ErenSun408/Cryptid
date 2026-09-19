#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include <string>
#include <string_view>
#include <system_error>
#include <filesystem>

#include "platform/windows/vikey_configuration/installers.h"
#include "platform/windows/vikey_configuration/setup_policy.h"
#include "usb_vault/volume/volume_provider.h"

namespace {
using configuration::setup::Result;

Result webview() {
  LPWSTR version{};
  const HRESULT status = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
  const bool available = SUCCEEDED(status) && version && *version &&
      std::wstring_view(version) != L"0.0.0.0";
  CoTaskMemFree(version);
  if (available) return Result::ready;
  if (status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || SUCCEEDED(status))
    return Result::missing;
  return Result::unavailable;
}

Result veracrypt() {
  try {
    return usb_vault::volume::VolumeProvider{}.driver_version() == 0x126
        ? Result::ready : Result::incompatible;
  } catch (const std::system_error& error) {
    if (error.code().category() != std::system_category() ||
        (error.code().value() != ERROR_FILE_NOT_FOUND && error.code().value() != ERROR_PATH_NOT_FOUND))
      return Result::unavailable;
  }
  // Do not overwrite an existing but inaccessible or stopped driver.
  const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) return Result::unavailable;
  const auto service = OpenServiceW(manager, L"veracrypt", SERVICE_QUERY_STATUS);
  const DWORD error = service ? ERROR_SUCCESS : GetLastError();
  if (service) CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return error == ERROR_SERVICE_DOES_NOT_EXIST ? Result::missing : Result::unavailable;
}

std::filesystem::path directory() {
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) throw std::runtime_error("setup path unavailable");
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}

Result install(bool browser) {
  const auto result = browser ? configuration::install_webview(directory(), true)
                              : configuration::install_veracrypt(directory(), true);
  switch (result.state) {
    case configuration::InstallState::success: return Result::ready;
    case configuration::InstallState::reboot_required: return Result::restart;
    case configuration::InstallState::cancelled: return Result::cancelled;
    case configuration::InstallState::failed: return Result::failed;
  }
  return Result::failed;
}
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR command, _In_ int) {
  if (!SetDllDirectoryW(L"")) return static_cast<int>(Result::unavailable);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return static_cast<int>(Result::unavailable);
  Result result = Result::bad_arguments;
  try {
    const std::wstring_view mode(command);
    if (mode == L"--webview")
      result = configuration::setup::ensure_dependency(webview, [] { return install(true); });
    else if (mode == L"--veracrypt")
      result = configuration::setup::ensure_dependency(veracrypt, [] { return install(false); });
    else if (mode == L"--probe-webview") result = webview();
    else if (mode == L"--probe-veracrypt") result = veracrypt();
  } catch (...) { result = Result::failed; }
  CoUninitialize();
  return static_cast<int>(result);
}
