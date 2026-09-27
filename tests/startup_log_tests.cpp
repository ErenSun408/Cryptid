#include "platform/windows/startup_log.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <regex>
#include <algorithm>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace usb_vault::platform::windows;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(file.good(), "cannot read log");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void verify_card_snapshots(const fs::path& root) {
  using usb_vault::card::FailureDiagnostic;
  const FailureDiagnostic failure{
      .primary = {.code = 2011, .reason = 1, .native_kind = 1, .native_code = 5, .step = 56},
      .cleanup = {.code = 2015, .reason = 1, .native_kind = 1, .native_code = 32}};
  const auto count = [](const std::string& text, const std::string& needle) {
    size_t result = 0;
    for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++result;
    return result;
  };
  for (unsigned short scenario = 0; scenario < 7; ++scenario) {
    SYSTEMTIME time{2026, 9, 0, 22, 12, 0, scenario, 0};
    wchar_t name[80]{};
    swprintf_s(name, L"runtime-log-20260922-1200%02u.txt", static_cast<unsigned int>(scenario));
    const auto path = root / name;
    StartupLog log;
    require(log.open(root, time), "cannot open snapshot fixture");
    log.record_card_state(scenario == 1 ? failure : FailureDiagnostic{}, true);
    auto final = failure;
    size_t expected = 1;
    if (scenario == 0 || scenario == 1) {
      // Arrival (direct or deferred) and final snapshot must not duplicate a fault.
      log.record_card_state(failure);
      log.record_card_state(failure);
    } else if (scenario == 2) {
      // A fault first observed at page-ready must still be logged.
    } else if (scenario == 3) {
      final = {};
      expected = 0; // Normal no-card startup.
    } else if (scenario == 4) {
      log.record_card_state(failure);
      final.cleanup.native_code = 33;
      log.record_card_state(final);
      final.primary.step = 58;
      log.record_card_state(final);
      expected = 3; // Both cleanup and security-step changes are material.
    } else if (scenario == 5) {
      log.record_card_state(failure);
      log.record_card_state({}); // Removal/recovery resets duplicate suppression.
      log.record_card_state(failure);
      expected = 2;
    } else {
      for (int i = 0; i < 1000; ++i) log.record(StartupStep::webview, StartupOutcome::begin);
      log.record_card_state(failure); // Capped notification cannot consume final snapshot.
    }
    log.finish_page(final);
    const auto text = read(path);
    require(count(text, "错误码：2011") == expected, "card failure lost or duplicated");
    const auto ready = text.find("[完成] 页面就绪");
    const auto end = text.find("[完成] 日志结束");
    require(ready != std::string::npos && end > ready, "final markers missing or reordered");
    if (expected) {
      require(text.rfind("错误码：2011") < ready &&
          text.find("安全步骤码：56；原因码：1；Win32 错误：5") != std::string::npos &&
          text.find("附加清理错误码：2015") != std::string::npos,
          "final card diagnostic lost fields or followed page-ready");
    }
    require(text.size() < 65536 && std::count(text.begin(), text.end(), '\n') <= 129,
        "snapshot exceeded log budget");
    log.record_card_state(failure);
    log.finish_page(failure);
    require(read(path) == text, "runtime card event reopened completed startup log");
    fs::remove(path);
  }
}
// The runtime half of the log: the file outlives the startup section when it is
// published, takes events from several threads intact, describes failures with
// their codes and moves to a new part instead of dropping lines.
void verify_runtime_log(const fs::path& root) {
  namespace diagnostics = usb_vault::diagnostics;
  const auto lines_of = [](const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t end; (end = text.find("\r\n", start)) != std::string::npos; start = end + 2)
      lines.push_back(text.substr(start, end - start));
    return lines;
  };
  const SYSTEMTIME time{2026, 9, 0, 26, 9, 30, 0, 0};
  const auto path = root / L"runtime-log-20260926-093000.txt";
  {
    StartupLog log;
    require(log.open(root, time), "cannot open runtime fixture");
    log.continue_as_runtime_log();
    log.finish();
    log.record(StartupStep::failure, StartupOutcome::failed, {.primary = {.code = 3005}});
#if USB_VAULT_DEBUG_LOG
    diagnostics::debug_log("测试", "运行期事件", "路径：D:\\vault\\a.hc");
    try {
      throw std::system_error(std::make_error_code(std::errc::file_exists), "target exists");
    } catch (...) {
      diagnostics::debug_log_exception("测试", "系统错误");
    }
    try {
      throw std::runtime_error("plain failure");
    } catch (...) {
      diagnostics::debug_log_exception("测试", "普通异常", "盘符：X:");
    }
    try {
      throw 7;
    } catch (...) {
      diagnostics::debug_log_exception("测试", "未知异常");
    }
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 8; ++writer) {
      writers.emplace_back([writer] {
        for (int index = 0; index < 200; ++index)
          diagnostics::debug_log("并发", "写入", "写入者：" + std::to_string(writer));
      });
    }
    for (auto& writer : writers) writer.join();
#endif
    const auto text = read(path);
    require(text.starts_with("\xEF\xBB\xBFUSB Vault 运行日志"), "runtime log header missing");
    require(text.find("[失败] 启动错误") == std::string::npos,
            "the startup section accepted a record after it ended");
    require(text.find("日志结束") == std::string::npos,
            "the end marker was written before the runtime log closed");
#if USB_VAULT_DEBUG_LOG
    const auto runtime = text.find("[测试] 运行期事件；路径：D:\\vault\\a.hc");
    require(runtime != std::string::npos &&
            std::regex_search(text, std::regex(R"(\[线程 \d+\] \[测试\] 运行期事件)")),
            "runtime events did not follow the startup section in the same file");
    require(text.find("[测试] 系统错误；异常类型：class std::system_error；错误类别：generic；错误码：17（0x00000011）；原文：target exists") !=
                std::string::npos,
            "a system error lost its category, code or text");
    require(text.find("[测试] 普通异常；盘符：X:；异常类型：class std::runtime_error；原文：plain failure") !=
                std::string::npos,
            "a plain exception lost its detail, type or text");
    require(text.find("[测试] 未知异常；异常类型：未知（非 std::exception）") != std::string::npos,
            "an unknown exception was not described");
    size_t concurrent = 0;
    for (const auto& line : lines_of(text)) {
      if (line.find("[并发]") == std::string::npos) continue;
      require(std::regex_match(line, std::regex(
                  R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[线程 \d+\] \[并发\] 写入；写入者：[0-7])")),
              "a concurrent line was torn or interleaved");
      ++concurrent;
    }
    require(concurrent == 1600, "concurrent lines were lost");
#endif
  }
  require(diagnostics::runtime_log_slot().load() == nullptr,
          "the runtime log stayed published after its owner went away");
  {
    const auto closed = read(path);
    require(closed.ends_with("[完成] 日志结束\r\n") &&
                closed.find("日志结束") == closed.rfind("日志结束"),
            "the runtime log did not end with exactly one end marker");
  }
  diagnostics::debug_log("测试", "无主之后");
  require(read(path).find("无主之后") == std::string::npos,
          "an event reached a file whose owner had gone");
  fs::remove(path);

  // A small part size forces rolling; every line survives across the parts.
  const SYSTEMTIME rolled{2026, 9, 0, 26, 9, 31, 0, 0};
  {
    diagnostics::DebugLogFile file(4096);
    require(file.open(root, rolled), "cannot open rolling fixture");
    const std::string line(62, 'x');
    for (int index = 0; index < 200; ++index) file.write(line + "\r\n");
    require(file.error() == 0, "rolling reported an error");
  }
  size_t survived = 0;
  for (unsigned int part = 1; part <= 9; ++part) {
    wchar_t name[80]{};
    if (part == 1) swprintf_s(name, L"runtime-log-20260926-093100.txt");
    else swprintf_s(name, L"runtime-log-20260926-093100-p%02u.txt", part);
    if (!fs::exists(root / name)) continue;
    const auto text = read(root / name);
    require(text.size() <= 4096, "a part exceeded its size");
    require(text.starts_with("\xEF\xBB\xBFUSB Vault 运行日志\r\n"),
            "a part lacks the plain log header");
    for (const auto& line : lines_of(text)) survived += line == std::string(62, 'x') ? 1 : 0;
    fs::remove(root / name);
  }
  require(survived == 200, "rolling dropped lines");

  // A helper names its own file; an empty prefix is refused.
  {
    const SYSTEMTIME named{2026, 9, 0, 26, 9, 32, 0, 0};
    diagnostics::DebugLogFile helper_file;
    require(helper_file.open(root, named, L"creator-log-42"), "cannot open a prefixed log");
    helper_file.close();
    const auto helper_path = root / L"creator-log-42-20260926-093200.txt";
    require(fs::exists(helper_path) &&
                read(helper_path) == "\xEF\xBB\xBFUSB Vault 运行日志\r\n",
            "the prefix did not name the file or changed its header");
    fs::remove(helper_path);
    diagnostics::DebugLogFile unnamed;
    require(!unnamed.open(root, named, L"") && unnamed.error() == ERROR_INVALID_NAME,
            "an empty prefix was accepted");
  }

  require(diagnostics::flag_text(true) == "是" && diagnostics::flag_text(false) == "否" &&
              diagnostics::hex_text(0x80070005u) == "0x80070005" &&
              diagnostics::drive_text(L'x') == "X:" && diagnostics::drive_text(wchar_t{}) == "无",
          "a shared log formatter produced the wrong text");
  // A trace never disturbs the last Win32 error of the code around it.
  ::SetLastError(1234);
  USB_VAULT_TRACE("测试", "保留最近错误", std::string("细节"));
  require(::GetLastError() == 1234, "a trace changed the last Win32 error");
  try {
    throw std::runtime_error("handled");
  } catch (...) {
    ::SetLastError(5678);
    USB_VAULT_TRACE_FAILURE("测试", "保留最近错误", std::string("细节"));
    require(::GetLastError() == 5678, "a failure trace changed the last Win32 error");
  }
  // A detail that cannot be built must never escape the trace statement.
  USB_VAULT_TRACE("测试", "细节构造失败",
                  (throw std::runtime_error("detail failed"), std::string()));
  try {
    throw std::runtime_error("handled");
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("测试", "细节构造失败",
                            (throw std::runtime_error("detail failed"), std::string()));
  }
}

void verify_page_quota(const fs::path& root) {
  using usb_vault::diagnostics::DebugLogFile;
  const SYSTEMTIME time{2026, 9, 0, 27, 15, 0, 0, 0};
  {
    // Force rotation and concurrent callers. Neither may bypass the quota.
    DebugLogFile file(512);
    require(file.open(root, time, L"page-quota"), "page quota fixture open failed");
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
      for (int j = 0; j < 60; ++j) file.write_page_event("[页面] quota-event\r\n", 100);
    });
    for (auto& worker : workers) worker.join();
    file.write_page_event("[页面] before-boundary\r\n", 60099);
    file.write_page_event("[页面] stale-clock\r\n", 99);
    file.write_page_event("[页面] at-boundary\r\n", 60100);
    file.flush_page_events();
    file.flush_page_events();
  }
  std::string all;
  std::vector<fs::path> quota_files;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (!entry.path().filename().wstring().starts_with(L"page-quota-")) continue;
    all += read(entry.path());
    quota_files.push_back(entry.path());
  }
#if USB_VAULT_DEBUG_LOG
  std::size_t count = 0;
  for (std::size_t at = 0; (at = all.find("quota-event", at)) != std::string::npos; ++at) ++count;
  require(count == 200 && all.find("before-boundary") == std::string::npos &&
          all.find("at-boundary") != std::string::npos && all.find("stale-clock") == std::string::npos,
          "page rate limit failed across threads, rotation or window boundary");
  const auto summary = all.find("[页面] 限流；已丢弃 42 条\r\n");
  require(summary != std::string::npos && all.find("已丢弃") == all.rfind("已丢弃"),
          "page quota summary was missing or duplicated");
#else
  require(all.find("[页面]") == std::string::npos, "disabled quota wrote page events");
#endif
  const auto final_path = root / L"runtime-log-20260927-150000.txt";
  {
    StartupLog log;
    require(log.open(root, time), "page shutdown fixture open failed");
    log.continue_as_runtime_log();
    auto* file = usb_vault::diagnostics::runtime_log_slot().load();
    for (int i = 0; i < 202; ++i) file->write_page_event("[页面] shutdown-event\r\n", 100);
    log.finish();
  }
  const auto final = read(final_path);
  require(final.ends_with("[完成] 日志结束\r\n"), "page quota summary followed the end marker");
#if USB_VAULT_DEBUG_LOG
  require(final.find("[页面] 限流；已丢弃 2 条\r\n") != std::string::npos,
          "normal shutdown lost the pending page drop count");
#endif
  for (const auto& path : quota_files) fs::remove(path);
  fs::remove(final_path);
}

int main() {
  const auto root = fs::current_path() / (L"startup-log-tests-" + std::to_wstring(::GetCurrentProcessId()) +
      L"-" + std::to_wstring(::GetTickCount64()));
  try {
    require(fs::create_directory(root), "fixture already exists");
    verify_card_snapshots(root);
    verify_runtime_log(root);
    verify_page_quota(root);
    const SYSTEMTIME time{2026, 9, 0, 20, 17, 12, 34, 0};
    const auto first = root / L"runtime-log-20260920-171234.txt";
    const auto second = root / L"runtime-log-20260920-171234-01.txt";
    StartupLog log;
    require(log.open(root, time), "cannot create startup log");
    StartupLog collision;
    require(collision.open(root, time), "same-second launch failed instead of choosing suffix");
    collision.finish();
    require(fs::exists(first) && fs::exists(second), "timestamp naming failed");
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2019, .reason = 18, .native_kind = 0, .native_code = 0xdeadbeef},
         .cleanup = {.code = 2015, .reason = 1, .native_kind = 1, .native_code = 5}});
    const auto flushed = read(first);
    require(flushed.starts_with("\xEF\xBB\xBF") &&
            std::regex_search(flushed, std::regex(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\])")),
            "UTF-8 BOM or local millisecond timestamp missing");
    require(flushed.find("[失败] 会话初始化与首次安全卡探测；错误码：2019；原因码：18") != std::string::npos &&
            flushed.find("附加清理错误码：2015；原因码：1；Win32 错误：5") != std::string::npos,
            "failure diagnostics not flushed or cleanup overwritten");
    require(flushed.find("3735928559") == std::string::npos, "untyped IPC detail leaked");
    log.record(StartupStep::session, StartupOutcome::completed,
        {.primary = {.code = 0, .native_kind = 1, .native_code = 0xdeadbeef}});
    require(read(first).find("3735928559") == std::string::npos, "success payload leaked into diagnostics");
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2011, .reason = 1, .native_kind = 1, .native_code = 5, .step = 56},
         .cleanup = {.code = 2015, .reason = 1, .native_kind = 1, .native_code = 32}});
    require(read(first).find("错误码：2011；安全步骤码：56；原因码：1；Win32 错误：5；附加清理错误码：2015") != std::string::npos,
            "security step/system error/cleanup did not survive log output");
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2011, .reason = 1, .native_kind = 0, .native_code = 0xdeadbeef, .step = 58}});
    require(read(first).find("错误码：2011；安全步骤码：58；原因码：1\r\n") != std::string::npos,
            "logical rejection included stale system error");
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2011, .reason = 1, .native_kind = 2, .native_code = 0x80070005, .step = 52}});
    require(read(first).find("安全步骤码：52；原因码：1；HRESULT：2147942405") != std::string::npos,
            "HRESULT logging lost unsigned value");
    log.record(StartupStep::session, StartupOutcome::completed,
        {.primary = {.step = 123456}});
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2011, .step = 123456}, .cleanup = {.code = 2015, .step = 123456}});
    require(read(first).find("123456") == std::string::npos,
            "success, unknown or non-security step leaked");
    log.finish();
    const auto finished = read(first);
    require(finished.find("code=") == std::string::npos && finished.find("错误码：0") == std::string::npos &&
            finished.find("[完成] 会话初始化与首次安全卡探测\r\n") != std::string::npos,
            "successful events contain numeric diagnostic clutter");
    log.record(StartupStep::failure, StartupOutcome::failed, {.primary = {.code = 3005}});
    log.finish();
    require(read(first) == finished, "runtime activity appended after startup ended");
    StartupLog bounded;
    require(bounded.open(root, time), "cannot create bounded log");
    for (int index = 0; index < 1000; ++index) bounded.record(StartupStep::webview, StartupOutcome::begin);
    bounded.finish();
    const auto third = root / L"runtime-log-20260920-171234-02.txt";
    const auto limited = read(third);
    require(limited.size() < 65536 && std::count(limited.begin(), limited.end(), '\n') <= 129 &&
            limited.find("日志结束") != std::string::npos,
            "log exceeded fixed record budget");
    StartupLog unavailable;
    require(!unavailable.open(root / L"missing", time) && unavailable.error() != 0,
            "unwritable destination falsely reported success");
    unavailable.record(StartupStep::entry, StartupOutcome::begin);
    unavailable.finish();
    require(!unavailable.open(first, time), "regular file accepted as log directory");
    require(read(first) == finished, "failed logging overwrote existing file");
    // Remove only exact files owned by this fixture; no recursive deletion.
    fs::remove(first); fs::remove(second); fs::remove(third); fs::remove(root);
    std::cout << "startup_log_tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
