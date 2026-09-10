#pragma once
#include "checks.h"
#include <filesystem>

namespace configuration {
std::filesystem::path executable_directory();
bool diagnose(const Report& report);
void launch_application();
}
