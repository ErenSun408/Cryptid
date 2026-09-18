#pragma once
#include <Windows.h>

namespace configuration {
// Runs before the product session exists. True means the user chose to
// continue after successful checks. No separate configuration process starts.
bool run_window(HINSTANCE instance, int show_command, bool in_application);
}
