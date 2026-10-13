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
#include "platform/windows/vikey_configuration/driver_cleanup.h"
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

std::filesystem::path directory() {
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size())
    throw std::system_error(static_cast<int>(size ? ERROR_INSUFFICIENT_BUFFER : GetLastError()),
        std::system_category(), "setup path unavailable");
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
  const bool dll_directory_secured = SetDllDirectoryW(L"") != FALSE;
  const DWORD directory_error = dll_directory_secured ? ERROR_SUCCESS : GetLastError();
  configuration::driver::Diagnostic diagnostic{};
  std::wstring diagnostic_token;
  bool driver_mode = false;
  bool com_initialized = false;
  Result result = Result::bad_arguments;
  try {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return static_cast<int>(Result::bad_arguments);
    const std::wstring mode = argc >= 2 ? argv[1] : L"";
    const std::filesystem::path application = argc == 3 ? argv[2] : L"";
    const bool cleanup_profile = mode == L"--cleanup-webview-profile" && argc == 3;
    const bool cleanup_application = mode == L"--cleanup-application-data" && argc == 3;
    const bool single_mode = argc == 2;
    const bool diagnostic_child = mode == L"--install-driver-elevated" && argc == 4 &&
        std::wstring_view(argv[2]) == L"--diagnostic-pipe" &&
        configuration::driver::valid_diagnostic_token(argv[3]);
    if (diagnostic_child) diagnostic_token = argv[3];
    LocalFree(argv);
    driver_mode = mode == L"--probe-veracrypt" || mode == L"--veracrypt" || mode == L"--install-driver-elevated";
    diagnostic.step = configuration::driver::Step::helper_initialization;
    if (!dll_directory_secured) {
      diagnostic.kind = configuration::driver::CodeKind::win32;
      diagnostic.code = directory_error;
      result = Result::unavailable;
    } else if (const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); FAILED(com)) {
      diagnostic.kind = configuration::driver::CodeKind::hresult;
      diagnostic.code = static_cast<std::uint32_t>(com);
      result = Result::unavailable;
    } else {
      com_initialized = true;
      diagnostic = {};
      if (cleanup_application)
        result = configuration::webview_cleanup::clean_application_data(application);
      else if (cleanup_profile)
        result = configuration::webview_cleanup::clean_profile(application);
      else if (!single_mode && !diagnostic_child) result = Result::bad_arguments;
      else if (mode == L"--probe-driver-diagnostics") result = Result::ready;
      else if (mode == L"--probe-uninstall-cleanup") result = Result::ready;
      else if (mode == L"--probe-application-cleanup") result = Result::ready;
      else if (mode == L"--probe-driver-cleanup") result = Result::ready;
      else if (mode == L"--uninstall-veracrypt")
        result = configuration::driver_cleanup::request_uninstall(directory() / L"usb_vault_setup_helper.exe");
      else if (mode == L"--uninstall-driver-elevated")
        result = configuration::driver_cleanup::uninstall_elevated();
      else if (mode == L"--uninstall-webview")
        result = configuration::webview_cleanup::uninstall_runtime();
      else if (mode == L"--webview")
        result = configuration::setup::ensure_dependency(webview, [] { return install_webview(); });
      else if (mode == L"--veracrypt") {
        diagnostic.step = configuration::driver::Step::system_path;
        result = configuration::driver::request_install(directory() / L"usb_vault_setup_helper.exe", &diagnostic);
      } else if (mode == L"--install-driver-elevated") {
        diagnostic.step = configuration::driver::Step::system_path;
        result = configuration::driver::install_elevated(directory(), &diagnostic);
      } else if (mode == L"--probe-webview") result = webview();
      else if (mode == L"--probe-veracrypt") result = configuration::driver::probe(&diagnostic);
    }
  } catch (const std::system_error& error) {
    configuration::driver::capture_exception(diagnostic, error); result = Result::failed;
  } catch (...) { result = Result::failed; }
  if (com_initialized) CoUninitialize();
  if (!diagnostic_token.empty())
    static_cast<void>(configuration::driver::send_diagnostic(diagnostic_token, result, diagnostic));
  else if (driver_mode && result != Result::ready) {
    try {
      const auto text = configuration::driver::format_diagnostic(diagnostic);
      DWORD count{};
      static_cast<void>(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(),
          static_cast<DWORD>(text.size()), &count, nullptr));
    } catch (...) { /* Diagnostics never change the operation result. */ }
  }
  return static_cast<int>(result);
}
