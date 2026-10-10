#pragma once
#include "setup_policy.h"
#include <filesystem>
#include <string_view>

namespace configuration::webview_cleanup {
// No caller-supplied executable or command line is ever accepted.
bool valid_version(std::wstring_view version) noexcept;
std::filesystem::path setup_path(const std::filesystem::path& root,
                                 std::wstring_view version);
setup::Result uninstall_runtime() noexcept;
// Only EBWebView inside the reserved USB Vault.exe.WebView2 application profile.
// Does not touch arbitrary profile overrides or another application's data.
setup::Result clean_profile(const std::filesystem::path& application) noexcept;
bool microsoft_signed_file(const std::filesystem::path& path) noexcept;

// Injectable orchestration shared by the real adapter and deterministic tests.
template <class Inspect, class Run, class Absent>
setup::Result remove_runtime(Inspect inspect, Run run, Absent absent) {
  const auto before = inspect();
  if (before == setup::Result::missing) return setup::Result::ready;
  if (before != setup::Result::ready) return before;
  const auto result = run();
  if (result != setup::Result::ready) return result;
  return absent() ? setup::Result::ready : setup::Result::failed;
}
}
