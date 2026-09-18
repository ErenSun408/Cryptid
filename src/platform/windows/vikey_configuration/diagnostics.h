#pragma once
#include "checks.h"
#include <filesystem>

namespace configuration {
std::filesystem::path executable_directory();
bool diagnose(const Report& report, bool include_card = true);
// Silent start check. It never touches the security card, so starting the
// product cannot consume a card attempt on its own.
bool environment_ready();
void launch_application();
}
