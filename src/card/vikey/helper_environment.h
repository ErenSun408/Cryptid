#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace usb_vault::card::vikey::helper {
enum class EnvironmentBoundary { invalid, zero_network_app_container, network_enabled_development };
struct EnvironmentCheck {
  EnvironmentBoundary boundary{EnvironmentBoundary::invalid};
  std::uint32_t step{};
  std::uint32_t native{};
};
// Views borrow only the OS block. No environment names or values cross IPC.
class EnvironmentInspection {
 public:
  void add(std::wstring_view entry) noexcept {
    if (failure_) return;
    const auto split = entry.find(L'=');
    if (split == std::wstring_view::npos || split == 0) { failure_ = 90; return; }
    const auto name = entry.substr(0, split);
    for (std::size_t i = 0; i < names_.size(); ++i) {
      if (name == names_[i]) {
        if (seen_[i]) { failure_ = 93; return; }
        seen_[i] = true;
        values_[i] = entry.substr(split + 1);
        return;
      }
      if (same_ascii_case_insensitive(name, names_[i])) { failure_ = 92; return; }
    }
    failure_ = 91;
  }
  [[nodiscard]] EnvironmentCheck finish() const noexcept {
    if (failure_) return {.step = failure_};
    for (std::size_t i = 0; i < 3; ++i) {
      if (!seen_[i]) return {.step = static_cast<std::uint32_t>(94 + i * 2)};
      if (values_[i].empty()) return {.step = static_cast<std::uint32_t>(95 + i * 2)};
    }
    if (values_[1] != values_[2]) return {.step = 100};
    const auto base = values_[0];
    const auto temp = values_[1];
    if (!temp.starts_with(base) || temp.substr(base.size()) != L"\\Temp") return {.step = 101};
    if (!seen_[3] && !seen_[4]) return {.boundary = EnvironmentBoundary::zero_network_app_container};
    if (!seen_[3]) return {.step = 102};
    if (values_[3].empty()) return {.step = 103};
    if (!seen_[4]) return {.step = 104};
    if (values_[4].empty()) return {.step = 105};
    if (values_[3] != values_[4]) return {.step = 106};
    return {.boundary = EnvironmentBoundary::network_enabled_development};
  }
 private:
  static bool same_ascii_case_insensitive(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
    for (std::size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
  }
  static constexpr std::array<std::wstring_view, 5> names_{L"LOCALAPPDATA", L"TEMP", L"TMP", L"SystemRoot", L"WINDIR"};
  std::array<std::wstring_view, 5> values_{};
  std::array<bool, 5> seen_{};
  std::uint32_t failure_{};
};
// Injectable OS boundary; production and tests run this exact implementation.
// Non-allocating checks ensure the block is released even for invalid input.
template<class Get, class Free, class LastError>
[[nodiscard]] EnvironmentCheck inspect_helper_environment(Get get, Free free, LastError last_error) noexcept {
  auto* block = get();
  if (!block) return {.step = 107, .native = last_error()};
  EnvironmentInspection inspection;
  for (const wchar_t* entry = block; *entry;) {
    const std::wstring_view value(entry);
    inspection.add(value);
    entry += value.size() + 1;
  }
  auto result = inspection.finish();
  if (!free(block)) {
    const auto error = last_error();
    // Preserve the first rejection if release also fails.
    if (!result.step) result = {.step = 108, .native = error};
  }
  return result;
}
} // namespace usb_vault::card::vikey::helper
