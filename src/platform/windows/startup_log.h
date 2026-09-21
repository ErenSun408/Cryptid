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
    started_ = ::GetTickCount64();
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
      constexpr char header[] = "USB Vault startup log; format=1; filename_time=local\r\n";
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
    char text[512]{};
    const int length = sprintf_s(text,
        "seq=%u elapsed_ms=%llu step=%u outcome=%u code=%u reason=%u native_kind=%u native_code=%u cleanup_code=%u cleanup_reason=%u cleanup_kind=%u cleanup_native=%u event_kind=%u event_code=%u substep=%u\r\n",
        sequence_++, ::GetTickCount64() - started_, static_cast<unsigned int>(step),
        static_cast<unsigned int>(outcome), primary.code, primary.reason, primary.native_kind,
        primary.native_code, cleanup.code, cleanup.reason, cleanup.native_kind,
        cleanup.native_code, event_kind, event_code, substep);
    if (length > 0) write(text, static_cast<DWORD>(length));
    else { error_ = ERROR_INVALID_DATA; close(); }
  }
  void finish() noexcept {
    if (file_ == INVALID_HANDLE_VALUE) return;
    record(StartupStep::end, StartupOutcome::completed);
    close();
  }
  [[nodiscard]] DWORD error() const noexcept { return error_; }
 private:
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
  ULONGLONG started_{};
};
}  // namespace usb_vault::platform::windows
