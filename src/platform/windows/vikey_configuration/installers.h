#pragma once
#include "card/verified_local_artifact.h"
#include <filesystem>

namespace configuration {
inline constexpr wchar_t kWebViewInstallerDigest[] = L"E7FA35755196AD9223596EF021A1CE6799509142EAA40BA35F634026BE50B831";
inline constexpr wchar_t kVeraCryptDriverDigest[] = L"1F0C6DB3559D1356C38A1486A967CD90DB5E6202E433FEA1DFE510DDB884FFB6";
usb_vault::card::VerifiedLocalArtifact verify_veracrypt_driver(
    const std::filesystem::path& path, bool allow_delete = false);
usb_vault::card::VerifiedLocalArtifact verify_webview_installer(
    const std::filesystem::path& path);
// Verification only: never starts a process. Keeps the exact reviewed bytes
// locked while the caller performs the separately requested installation.
enum class InstallState { success, cancelled, reboot_required, failed };
struct InstallResult { InstallState state; unsigned long exit_code; };
inline InstallResult classify_installer_exit(unsigned long code) {
  if (code == 0) return {InstallState::success, code};
  if (code == 3010 || code == 1641) return {InstallState::reboot_required, code};
  if (code == 1602 || code == 1223) return {InstallState::cancelled, code};
  return {InstallState::failed, code};
}
InstallResult install_webview(const std::filesystem::path& directory, bool silent = false);
}
