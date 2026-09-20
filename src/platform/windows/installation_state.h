#pragma once
#include <Windows.h>
#include <filesystem>
#include "card/capture_failure.h"

namespace usb_vault::platform::windows {
// Check before dependencies, credentials, card access, or WebView startup.
// Any reserved maintenance entry (including an inaccessible one) blocks use.
inline bool installation_ready_for_start(const std::filesystem::path& directory) {
  for (const auto* name : {L".usb-vault-update", L".usb-vault-setup.lock"}) {
    if (::GetFileAttributesW((directory / name).c_str()) != INVALID_FILE_ATTRIBUTES)
      return false;
    const auto error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
      usb_vault::card::throw_startup_failure(1004, 1, error);
  }
  return true;
}
}
