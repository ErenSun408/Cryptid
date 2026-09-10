#include <Windows.h>
#include <shellapi.h>
#include <wintrust.h>
#include <Softpub.h>
#include "installers.h"
#include "card/vikey/vikey_development_app_config.h"
#include <stdexcept>
#include <system_error>

namespace configuration {
usb_vault::card::VerifiedLocalArtifact verify_veracrypt_installer(
    const std::filesystem::path& path) {
  auto artifact = usb_vault::card::verify_local_artifact(path,
      usb_vault::card::vikey::development::parse_deployment_digest(
          L"DCA9DDEC7934FC0B1C25AC8984DB7D504BFE9F37C21861AC91C67A98ECD3964B"),
      128ULL * 1024 * 1024);
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = artifact.path().c_str();
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = ::WinVerifyTrust(nullptr, &action, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  static_cast<void>(::WinVerifyTrust(nullptr, &action, &trust));
  if (result != ERROR_SUCCESS) throw std::runtime_error("installer signature rejected");
  return artifact;
}

bool install_veracrypt(const std::filesystem::path& directory) {
  const auto installer = verify_veracrypt_installer(
      directory / L"prerequisites" / L"VeraCrypt Setup 1.26.29.exe");
  const auto parent = installer.path().parent_path();
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  execute.lpVerb = L"runas";
  execute.lpFile = installer.path().c_str();
  execute.lpDirectory = parent.c_str();
  execute.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&execute)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_CANCELLED) return false;
    throw std::system_error(static_cast<int>(error), std::system_category(), "start installer");
  }
  if (!execute.hProcess) throw std::runtime_error("installer process unavailable");
  const DWORD wait = ::WaitForSingleObject(execute.hProcess, INFINITE);
  ::CloseHandle(execute.hProcess);
  if (wait != WAIT_OBJECT_0) throw std::runtime_error("installer wait failed");
  return true;
}

void open_webview_download() {
  const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open",
      L"https://developer.microsoft.com/en-us/microsoft-edge/webview2/#download-section",
      nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) throw std::runtime_error("cannot open official download page");
}
}
