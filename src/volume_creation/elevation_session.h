#pragma once
#include <Windows.h>
#include <filesystem>
#include <string_view>
namespace usb_vault::volume_creation {
// Application lifetime grant. Failure never enables a per-operation fallback.
bool begin_elevation_session(const std::filesystem::path& verified_helper);
void end_elevation_session() noexcept;
namespace detail {
// Returns null only when no session was requested (standalone test/CLI clients).
HANDLE launch_session_helper(const std::filesystem::path& helper, std::wstring_view nonce);
int run_elevation_session(DWORD parent, std::wstring_view nonce) noexcept;
}
}
