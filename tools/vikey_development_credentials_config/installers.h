#pragma once
#include "card/verified_local_artifact.h"
#include <filesystem>

namespace configuration {
// Verification only: never starts a process. Keeps the exact reviewed bytes
// locked while the caller performs the separately requested installation.
usb_vault::card::VerifiedLocalArtifact verify_veracrypt_installer(
    const std::filesystem::path& path);
// Returns false if the user cancels UAC; true means the installer exited,
// not that the driver is installed or ready.
bool install_veracrypt(const std::filesystem::path& directory);
void open_webview_download();
}
