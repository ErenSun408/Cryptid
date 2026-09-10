#pragma once
#include "card/verified_local_artifact.h"
#include <filesystem>

namespace configuration {
inline constexpr wchar_t kWebViewInstallerDigest[] = L"E7FA35755196AD9223596EF021A1CE6799509142EAA40BA35F634026BE50B831";
usb_vault::card::VerifiedLocalArtifact verify_webview_installer(
    const std::filesystem::path& path);
// Verification only: never starts a process. Keeps the exact reviewed bytes
// locked while the caller performs the separately requested installation.
usb_vault::card::VerifiedLocalArtifact verify_veracrypt_installer(
    const std::filesystem::path& path);
// Returns false if the user cancels UAC; true means the installer exited,
// not that the driver is installed or ready.
bool install_veracrypt(const std::filesystem::path& directory);
bool install_webview(const std::filesystem::path& directory);
}
