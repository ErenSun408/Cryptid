#include <Windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <WebView2.h>
#include <string>
#include <string_view>
#include <system_error>
#include <filesystem>

#include "platform/windows/vikey_configuration/installers.h"
#include "platform/windows/vikey_configuration/driver_deployment.h"
#include "platform/windows/vikey_configuration/setup_policy.h"
#include "platform/windows/vikey_configuration/webview_cleanup.h"
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

Result veracrypt() { return configuration::driver::probe(); }

std::filesystem::path directory() {
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) throw std::runtime_error("setup path unavailable");
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}

Result install_webview() {
  const auto result = configuration::install_webview(directory(), true);
  switch (result.state) {
    case configuration::InstallState::success: return Result::ready;
    case configuration::InstallState::reboot_required: return Result::restart;
    case configuration::InstallState::cancelled: return Result::cancelled;
    case configuration::InstallState::failed: return Result::failed;
  }
  return Result::failed;
}
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
  if (!SetDllDirectoryW(L"")) return static_cast<int>(Result::unavailable);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return static_cast<int>(Result::unavailable);
  Result result = Result::bad_arguments;
  try {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { CoUninitialize(); return static_cast<int>(Result::bad_arguments); }
    const std::wstring mode = argc >= 2 ? argv[1] : L"";
    const std::filesystem::path application = argc == 3 ? argv[2] : L"";
    const bool cleanup_profile = mode == L"--cleanup-webview-profile" && argc == 3;
    const bool single_mode = argc == 2;
    LocalFree(argv);
    if (cleanup_profile)
      result = configuration::webview_cleanup::clean_profile(application);
    else if (!single_mode) result = Result::bad_arguments;
    else if (mode == L"--probe-uninstall-cleanup") result = Result::ready;
    else if (mode == L"--uninstall-webview")
      result = configuration::webview_cleanup::uninstall_runtime();
    else if (mode == L"--webview")
      result = configuration::setup::ensure_dependency(webview, [] { return install_webview(); });
    else if (mode == L"--veracrypt")
      result = configuration::driver::request_install(directory() / L"usb_vault_setup_helper.exe");
    else if (mode == L"--install-driver-elevated")
      result = configuration::driver::install_elevated(directory());
    else if (mode == L"--probe-webview") result = webview();
    else if (mode == L"--probe-veracrypt") result = veracrypt();
  } catch (...) { result = Result::failed; }
  CoUninitialize();
  return static_cast<int>(result);
}
