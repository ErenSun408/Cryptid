#pragma once
#include "card/process_security_failure.h"
#include <Windows.h>
#include "usb_vault/card/failure_diagnostic.h"
#include "usb_vault/diagnostics/debug_log.h"
#include <cstdio>
#include <filesystem>
#include <new>

namespace usb_vault::platform::windows {
enum class StartupStep : unsigned int {
  entry = 1, maintenance = 2, ole = 3, preflight = 4, window = 5,
  artifacts = 6, session = 7, webview = 8, ready = 9, failure = 10, end = 11, card_state = 12
};
enum class StartupOutcome : unsigned int { begin = 0, completed = 1, failed = 2 };

// UI-thread only. No text/payload input: success IPC detail can be an identity.
// Create-only, bounded, flushed on each record; logging never throws or retries
// a card operation. The startup section ends when the initial page is ready;
// once published as the runtime log, the same file stays open for the runtime
// events that follow, and the end marker is written only when it closes.
class StartupLog final {
 public:
  ~StartupLog() noexcept {
    if (diagnostics::runtime_log_slot().load() == &file_) diagnostics::set_runtime_log(nullptr);
    finish();
    end_log();
  }
  StartupLog() = default;
  StartupLog(const StartupLog&) = delete;
  StartupLog& operator=(const StartupLog&) = delete;
  bool open(const std::filesystem::path& directory) noexcept {
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    return open(directory, time);
  }
  bool open(const std::filesystem::path& directory, const SYSTEMTIME& time) noexcept {
    finish();
    end_log();
    error_ = 0;
    sequence_ = 0;
    last_card_failure_ = {};
    finalizing_page_ = false;
    finished_ = false;
    ended_ = false;
    if (!file_.open(directory, time)) return false;
    record(StartupStep::entry, StartupOutcome::begin);
    return error() == 0;
  }
  // Keeps the file open after the startup section ends and routes the
  // runtime events of this launch into it.
  void continue_as_runtime_log() noexcept {
    keep_open_ = true;
    diagnostics::set_runtime_log(&file_);
  }
  void record(StartupStep step, StartupOutcome outcome,
      card::FailureDiagnostic failure = {}, unsigned int event_kind = 0,
      unsigned int event_code = 0, unsigned int substep = 0) noexcept {
    // Reserve the final snapshot, page-ready marker and end marker.
    if (!file_.is_open() || error_ || (finished_ && step != StartupStep::end) || sequence_ >=
        (finalizing_page_ || step == StartupStep::end ? 128u : 125u)) return;
    const auto primary = clean(failure.primary);
    const auto cleanup = clean(failure.cleanup);
    if (event_kind > 2) { event_kind = 0; event_code = 0; }
    if (!event_kind) event_code = 0;
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    char text[512]{};
    size_t used = 0;
    bool valid = true;
    const auto append = [&](const char* format, auto... args) noexcept {
      if (!valid) return;
      const int length = std::snprintf(text + used, sizeof(text) - used, format, args...);
      if (length < 0 || static_cast<size_t>(length) >= sizeof(text) - used) valid = false;
      else used += static_cast<size_t>(length);
    };
    append("[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] %s",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, outcome == StartupOutcome::begin ? "开始" :
        outcome == StartupOutcome::failed ? "失败" : "完成", stage_name(step, substep));
    const auto detail = [&](const card::FailureDetail& value, const char* label) noexcept {
      if (!value.code) return;
      append("；%s：%u", label, value.code);
      if (value.step) append("；安全步骤码：%u", value.step);
      if (value.reason) append("；原因码：%u", value.reason);
      if (value.native_kind) append("；%s：%u", value.native_kind == 1 ? "Win32 错误" :
          value.native_kind == 2 ? "HRESULT" : "进程退出码", value.native_code);
    };
    detail(primary, "错误码");
    detail(cleanup, "附加清理错误码");
    if (event_kind) append("；事件类别：%u；事件码：%u", event_kind, event_code);
    if (step == StartupStep::webview && substep && substep != 1305 && substep != 1313)
      append("；步骤码：%u", substep);
    append("\r\n");
    ++sequence_;
    if (valid) file_.write(std::string_view(text, used));
    else { error_ = ERROR_INVALID_DATA; file_.close(); }
  }
  void record_card_state(card::FailureDiagnostic failure, bool initial = false) noexcept {
    if (!file_.is_open() || error_ || finished_) return;
    failure = {.primary = clean(failure.primary), .cleanup = clean(failure.cleanup)};
    const bool failed = failure.primary.code || failure.cleanup.code;
    const bool changed = !same(failure.primary, last_card_failure_.primary) ||
                         !same(failure.cleanup, last_card_failure_.cleanup);
    if (initial || (failed && changed)) {
      const auto before = sequence_;
      record(initial ? StartupStep::session : StartupStep::card_state,
          failed ? StartupOutcome::failed : StartupOutcome::completed, failure);
      if (before == sequence_) return; // Preserve a capped event for the final snapshot.
    }
    last_card_failure_ = failure;
  }
  void finish_page(card::FailureDiagnostic failure) noexcept {
    if (!file_.is_open() || finished_) return;
    finalizing_page_ = true;
    record_card_state(failure);
    record(StartupStep::ready, StartupOutcome::completed);
    finish();
    finalizing_page_ = false;
  }
  // Ends the startup section. A log that carries on as the runtime log stays
  // open and receives its end marker when it is closed.
  void finish() noexcept {
    if (!file_.is_open() || finished_) return;
    finished_ = true;
    if (!keep_open_) end_log();
  }
  [[nodiscard]] DWORD error() const noexcept { return error_ ? error_ : file_.error(); }
 private:
  static card::FailureDetail clean(card::FailureDetail value) noexcept {
    if (value.code < 1000 || value.code > 9999) return {};
    if (value.code != 2011 || value.step > card::kProcessSecurityStepCount) value.step = 0;
    if (value.reason > 18) value.reason = 0;
    if (value.native_kind < 1 || value.native_kind > 3) {
      value.native_kind = 0; value.native_code = 0;
    }
    return value;
  }
  static bool same(const card::FailureDetail& a, const card::FailureDetail& b) noexcept {
    return a.code == b.code && a.reason == b.reason && a.native_kind == b.native_kind &&
        a.native_code == b.native_code && a.step == b.step;
  }
  static const char* stage_name(StartupStep step, unsigned int substep) noexcept {
    switch (step) {
      case StartupStep::entry: return "应用启动";
      case StartupStep::maintenance: return "安装与维护状态检查";
      case StartupStep::ole: return "OLE 初始化";
      case StartupStep::preflight:
        switch (substep) {
          case 1: return "系统检查";
          case 2: return "应用组件检查";
          case 3: return "WebView 运行环境检查";
          case 4: return "驱动检查";
          case 5: return "凭据检查";
          default: return "启动前置检查";
        }
      case StartupStep::window: return "应用窗口创建";
      case StartupStep::artifacts: return "界面文件准备";
      case StartupStep::card_state: return "启动期间安全卡诊断";
      case StartupStep::session: return "会话初始化与首次安全卡探测";
      case StartupStep::webview:
        if (substep == 1305) return "WebView 控制器创建";
        if (substep == 1313) return "页面导航";
        return "WebView 页面初始化";
      case StartupStep::ready: return "页面就绪";
      case StartupStep::failure: return "启动错误";
      case StartupStep::end: return "日志结束";
    }
    return "未知启动阶段";
  }
  void end_log() noexcept {
    if (!file_.is_open() || ended_) return;
    file_.flush_page_events();
    record(StartupStep::end, StartupOutcome::completed);
    ended_ = true;
    file_.close();
  }
  diagnostics::DebugLogFile file_;
  DWORD error_{};
  bool finished_{};
  bool ended_{};
  bool keep_open_{};
  unsigned int sequence_{};
  card::FailureDiagnostic last_card_failure_{};
  bool finalizing_page_{};
};
}  // namespace usb_vault::platform::windows
