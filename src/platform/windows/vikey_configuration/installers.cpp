#include "card/diagnostic_error.h"
using namespace usb_vault::card;
#include <Windows.h>
#include <shellapi.h>
#include <wintrust.h>
#include <Softpub.h>
#include "installers.h"
#include "card/vikey/vikey_development_app_config.h"
#include <stdexcept>
#include <system_error>

namespace configuration {
static usb_vault::card::VerifiedLocalArtifact verify_installer(
    const std::filesystem::path& path, const wchar_t* digest) {
  auto artifact = usb_vault::card::verify_local_artifact(path,
      usb_vault::card::vikey::development::parse_deployment_digest(
          digest),
      512ULL * 1024 * 1024);
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
  if (result != ERROR_SUCCESS) throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::installer_signature, "verify installer signature", static_cast<std::uint32_t>(result), NativeCodeKind::hresult);
  return artifact;
}

static InstallResult run_installer(const usb_vault::card::VerifiedLocalArtifact& installer,
                                   const std::filesystem::path& executable,
                                   const wchar_t* parameters = nullptr) {
  const auto parent = installer.path().parent_path();
  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  execute.lpVerb = L"runas";
  execute.lpFile = executable.c_str();
  execute.lpParameters = parameters;
  execute.lpDirectory = parent.c_str();
  execute.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&execute)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_CANCELLED) return {InstallState::cancelled, error};
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::installer_start, "start installer", error, NativeCodeKind::win32);
  }
  if (!execute.hProcess) throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::installer_start, "installer process unavailable");
  const DWORD wait = ::WaitForSingleObject(execute.hProcess, INFINITE);
  DWORD exit_code = 0;
  const bool finished = wait == WAIT_OBJECT_0 && ::GetExitCodeProcess(execute.hProcess, &exit_code);
  const DWORD error = finished ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(execute.hProcess);
  if (!finished) throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::installer_wait, "wait installer", error, NativeCodeKind::win32);
  return classify_installer_exit(exit_code);
}

usb_vault::card::VerifiedLocalArtifact verify_veracrypt_driver(const std::filesystem::path& path) {
  return verify_installer(path, kVeraCryptDriverDigest);
}
usb_vault::card::VerifiedLocalArtifact verify_webview_installer(const std::filesystem::path& path) {
  return verify_installer(path, kWebViewInstallerDigest);
}
InstallResult install_webview(const std::filesystem::path& directory, const bool silent) {
  const auto installer = verify_webview_installer(directory / L"prerequisites" / L"MicrosoftEdgeWebView2RuntimeInstallerX64.exe");
  return run_installer(installer, installer.path(), silent ? L"/silent /install" : nullptr);
}
}
