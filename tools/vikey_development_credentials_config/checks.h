#pragma once

#include <array>
#include <functional>
#include <string>

namespace configuration {
enum class Check : unsigned int { system, package, webview, driver, credentials, device, card, count };
enum class State { waiting, running, passed, failed, skipped };
struct Result { State state{State::waiting}; std::wstring detail; };
constexpr auto count = static_cast<unsigned int>(Check::count);
using Probes = std::array<std::function<Result()>, count>;
using Report = std::function<void(unsigned int, const Result&)>;

// Card access requires verified binaries, saved credentials and a present
// device. Independent environment checks continue after a failure.
inline bool run_checks(const Probes& probes, const Report& report) {
  std::array<Result, count> results{};
  bool passed = true;
  for (unsigned int index = 0; index < count; ++index) {
    auto& result = results[index];
    if (index == static_cast<unsigned int>(Check::card) &&
        (results[1].state != State::passed || results[4].state != State::passed ||
         results[5].state != State::passed)) {
      result = {State::skipped, L"请先解决组件、凭据或插卡检查中的问题，再重新检测。"};
    } else {
      report(index, {State::running, L"正在检测…"});
      try { result = probes[index](); }
      catch (...) { result = {State::failed, L"检测失败，请检查环境后重试。"}; }
      if (result.state != State::passed && result.state != State::failed)
        result = {State::failed, L"检测未返回有效结果。"};
    }
    passed = passed && result.state == State::passed;
    report(index, result);
  }
  return passed;
}
}  // namespace configuration
