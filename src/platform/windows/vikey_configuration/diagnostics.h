#pragma once
#include "checks.h"
#include <filesystem>
#include <string>

namespace configuration {
std::filesystem::path executable_directory();
bool diagnose(const Report& report, bool include_card);

// One silent preflight check. This check never touches the security card;
// the subsequent product session probe may authenticate. Dependencies and saved
// credentials are reported apart because they are repaired in different
// places: dependencies belong to the installer, credentials to the setup page
// the product opens itself.
struct StartupInspection final {
  bool dependencies_ready{};
  bool credentials_ready{};
  // Why the first failing dependency failed. Empty when they are all usable.
  std::wstring detail;
  usb_vault::card::FailureDiagnostic failure{};
  usb_vault::card::FailureDiagnostic credential_failure{};
};
StartupInspection inspect_startup(const Report& observer = {});
}
