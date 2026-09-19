#pragma once
#include "checks.h"
#include <filesystem>
#include <string>

namespace configuration {
std::filesystem::path executable_directory();
bool diagnose(const Report& report, bool include_card);

// One silent start check. It never touches the security card, so starting the
// product cannot consume a PIN attempt on its own. Dependencies and saved
// credentials are reported apart because they are repaired in different
// places: dependencies belong to the installer, credentials to the setup page
// the product opens itself.
struct StartupInspection final {
  bool dependencies_ready{};
  bool credentials_ready{};
  // Why the first failing dependency failed. Empty when they are all usable.
  std::wstring detail;
};
StartupInspection inspect_startup() noexcept;
}
