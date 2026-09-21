#pragma once
#include <Windows.h>
#include "usb_vault/card/failure_diagnostic.h"
#include <cstdio>
#include <filesystem>
#include <new>

namespace usb_vault::platform::windows {
enum class StartupStep : unsigned int {
  entry = 1, maintenance = 2, ole = 3, preflight = 4, window = 5,
  artifacts = 6, session = 7, webview = 8, ready = 9, failure = 10, end = 11
};
enum class StartupOutcome : unsigned int { begin = 0, completed = 1, failed = 2 };

// UI-thread only. No text/payload input: success IPC detail can be an identity.
// Create-only, bounded, flushed on each record; logging never throws or retries
// a card operation. Logs intentionally stop when the initial page is ready.
class StartupLog final {
 public:
  ~StartupLog() noexcept { finish(); }
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
    error_ = 0;
    sequence_ = 0;
    try {
      const DWORD attributes = ::GetFileAttributesW(directory.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) { error_ = ::GetLastError(); return false; }
      if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error_ = ERROR_INVALID_NAME; return false;
      }
      for (unsigned int collision = 0; collision < 100; ++collision) {
        wchar_t name[96]{};
        if (collision == 0)
          swprintf_s(name, L"startup-log-%04u%02u%02u-%02u%02u%02u.txt",
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        else
          swprintf_s(name, L"startup-log-%04u%02u%02u-%02u%02u%02u-%02u.txt",
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, collision);
        file_ = ::CreateFileW((directory / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file_ != INVALID_HANDLE_VALUE) break;
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) { error_ = error; return false; }
      }
      if (file_ == INVALID_HANDLE_VALUE) { error_ = ERROR_FILE_EXISTS; return false; }
      constexpr char header[] = "\xEF\xBB\xBFUSB Vault 启动日志（格式 2，设备本地时间，精确到毫秒）\r\n";
      write(header, static_cast<DWORD>(sizeof(header) - 1));
      record(StartupStep::entry, StartupOutcome::begin);
      return error_ == 0;
    } catch (const std::bad_alloc&) { error_ = ERROR_NOT_ENOUGH_MEMORY; }
      catch (...) { error_ = ERROR_GEN_FAILURE; }
    close();
    return false;
  }
  void record(StartupStep step, StartupOutcome outcome,
      card::FailureDiagnostic failure = {}, unsigned int event_kind = 0,
      unsigned int event_code = 0, unsigned int substep = 0) noexcept {
    if (file_ == INVALID_HANDLE_VALUE || error_ || sequence_ >= 128) return;
    const auto clean = [](card::FailureDetail value) noexcept {
      if (value.code < 1000 || value.code > 9999) return card::FailureDetail{};
      if (value.reason > 18) value.reason = 0;
      if (value.native_kind < 1 || value.native_kind > 3) {
        value.native_kind = 0; value.native_code = 0;
      }
      return value;
    };
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
    if (valid) write(text, static_cast<DWORD>(used));
    else { error_ = ERROR_INVALID_DATA; close(); }
  }
  void finish() noexcept {
    if (file_ == INVALID_HANDLE_VALUE) return;
    record(StartupStep::end, StartupOutcome::completed);
    close();
  }
  [[nodiscard]] DWORD error() const noexcept { return error_; }
 private:
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
      case StartupStep::session: return "会话初始化与首次安全卡探测";
      case StartupStep::webview:
        if (substep == 1305) return "WebView 控制器创建";
        if (substep == 1313) return "页面导航";
        return "WebView 页面初始化";
      case StartupStep::ready: return "页面就绪";
      case StartupStep::failure: return "启动错误";
      case StartupStep::end: return "启动日志结束（不代表此前没有错误）";
    }
    return "未知启动阶段";
  }
  void write(const char* bytes, DWORD size) noexcept {
    DWORD written{};
    if (!::WriteFile(file_, bytes, size, &written, nullptr)) error_ = ::GetLastError();
    else if (written != size) error_ = ERROR_WRITE_FAULT;
    else if (!::FlushFileBuffers(file_)) error_ = ::GetLastError();
    if (error_) close();
  }
  void close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
      const HANDLE file = file_;
      file_ = INVALID_HANDLE_VALUE;
      if (!::CloseHandle(file) && !error_) error_ = ::GetLastError();
    }
  }
  HANDLE file_{INVALID_HANDLE_VALUE};
  DWORD error_{};
  unsigned int sequence_{};
};
}  // namespace usb_vault::platform::windows
