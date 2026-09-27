#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>

// Test-phase switch. When set, the runtime log also receives the detailed
// debug events: paths, container names, volume kinds and every failure with
// its codes. Those facts break the local-state rules of 8.4/8.6, so the
// switch must be turned off before a release build. Passwords, keys and card
// contents are never logged either way.
#ifndef USB_VAULT_DEBUG_LOG
#define USB_VAULT_DEBUG_LOG 0
#endif

namespace usb_vault::diagnostics {

// One plain-text UTF-8 file per launch, created beside the executable and
// shared by the startup record and the runtime events. Every write is flushed.
// A failure closes the file and is kept in error(); it never reaches the
// caller, because logging must not change what the application does.
class DebugLogFile final {
 public:
  static constexpr std::uint64_t kDefaultPartBytes = 8ULL * 1024ULL * 1024ULL;

  explicit DebugLogFile(const std::uint64_t part_bytes = kDefaultPartBytes) noexcept
      : part_bytes_(part_bytes) {}
  ~DebugLogFile() noexcept { close(); }
  DebugLogFile(const DebugLogFile&) = delete;
  DebugLogFile& operator=(const DebugLogFile&) = delete;

  // The prefix names the writer: the application keeps "runtime-log"; a helper
  // process uses its own so its file sits beside the application's.
  bool open(const std::filesystem::path& directory, const SYSTEMTIME& time,
            const std::wstring_view prefix = L"runtime-log") noexcept {
    std::lock_guard lock(mutex_);
    flush_page_events_locked();
    close_locked();
    page_count_ = 0;
    page_window_ = 0;
    error_ = 0;
    part_ = 1;
    written_ = 0;
    try {
      const DWORD attributes = ::GetFileAttributesW(directory.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) { error_ = ::GetLastError(); return false; }
      if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error_ = ERROR_INVALID_NAME;
        return false;
      }
      directory_ = directory;
      const std::wstring name_prefix(prefix);
      if (name_prefix.empty() || name_prefix.size() > 32) {
        error_ = ERROR_INVALID_NAME;
        return false;
      }
      for (unsigned int collision = 0; collision < 100; ++collision) {
        if (collision == 0)
          swprintf_s(stem_, L"%s-%04u%02u%02u-%02u%02u%02u", name_prefix.c_str(),
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        else
          swprintf_s(stem_, L"%s-%04u%02u%02u-%02u%02u%02u-%02u", name_prefix.c_str(),
              time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, collision);
        const DWORD created = create_locked(std::wstring(stem_) + L".txt");
        if (created == ERROR_SUCCESS) return write_header_locked();
        if (created != ERROR_FILE_EXISTS && created != ERROR_ALREADY_EXISTS) {
          error_ = created;
          return false;
        }
      }
      error_ = ERROR_FILE_EXISTS;
    } catch (const std::bad_alloc&) {
      error_ = ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
      error_ = ERROR_GEN_FAILURE;
    }
    close_locked();
    return false;
  }

  // Appends one complete line. A line that would push the current part past
  // its size moves the log to the next part file instead of being dropped.
  void write(const std::string_view line) noexcept {
    std::lock_guard lock(mutex_);
    append_locked(line);
  }

  // Both page bridges share this file and quota. Rotation does not reset it.
  // The explicit monotonic time also permits boundary tests without sleeping.
  void write_page_event(const std::string_view line,
                        const std::uint64_t now = ::GetTickCount64()) noexcept {
#if USB_VAULT_DEBUG_LOG
    std::lock_guard lock(mutex_);
    if (file_ == INVALID_HANDLE_VALUE || error_ || line.empty()) return;
    if (page_count_ == 0 || (now >= page_window_ && now - page_window_ >= 60000)) {
      flush_page_events_locked();
      page_window_ = now;
      page_count_ = 0;
    }
    if (page_count_ >= 200) {
      if (page_dropped_ != (std::numeric_limits<std::uint64_t>::max)()) ++page_dropped_;
      return;
    }
    ++page_count_;
    append_locked(line);
#else
    static_cast<void>(line);
    static_cast<void>(now);
#endif
  }

  void flush_page_events() noexcept {
    std::lock_guard lock(mutex_);
    flush_page_events_locked();
  }

 private:
  void append_locked(const std::string_view line) noexcept {
    if (file_ == INVALID_HANDLE_VALUE || error_ || line.empty()) return;
    if (written_ + line.size() > part_bytes_ && written_ > header_bytes_) {
      if (!next_part_locked()) return;
    }
    write_locked(line);
  }

 public:
  void close() noexcept {
    std::lock_guard lock(mutex_);
    flush_page_events_locked();
    close_locked();
  }
  [[nodiscard]] bool is_open() const noexcept {
    std::lock_guard lock(mutex_);
    return file_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] DWORD error() const noexcept {
    std::lock_guard lock(mutex_);
    return error_;
  }

 private:
  void flush_page_events_locked() noexcept {
    if (!page_dropped_) return;
    char line[160]{};
    std::snprintf(line, sizeof(line), "[页面] 限流；已丢弃 %llu 条\r\n",
                  static_cast<unsigned long long>(page_dropped_));
    page_dropped_ = 0;
    append_locked(line);
  }
  DWORD create_locked(const std::wstring& name) noexcept {
    try {
      file_ = ::CreateFileW((directory_ / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
          nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_WRITE_THROUGH, nullptr);
    } catch (...) {
      return ERROR_NOT_ENOUGH_MEMORY;
    }
    return file_ == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
  }
  bool write_header_locked() noexcept {
    constexpr std::string_view header = "\xEF\xBB\xBFUSB Vault 运行日志\r\n";
    header_bytes_ = header.size();
    write_locked(header);
    return error_ == 0;
  }
  bool next_part_locked() noexcept {
    close_locked();
    ++part_;
    written_ = 0;
    wchar_t name[96]{};
    swprintf_s(name, L"%s-p%02u.txt", stem_, part_);
    const DWORD created = create_locked(name);
    if (created != ERROR_SUCCESS) {
      error_ = created;
      return false;
    }
    return write_header_locked();
  }
  void write_locked(const std::string_view bytes) noexcept {
    DWORD written{};
    const DWORD size = static_cast<DWORD>(bytes.size());
    if (!::WriteFile(file_, bytes.data(), size, &written, nullptr)) error_ = ::GetLastError();
    else if (written != size) error_ = ERROR_WRITE_FAULT;
    else if (!::FlushFileBuffers(file_)) error_ = ::GetLastError();
    if (error_) close_locked();
    else written_ += size;
  }
  void close_locked() noexcept {
    if (file_ == INVALID_HANDLE_VALUE) return;
    const HANDLE file = file_;
    file_ = INVALID_HANDLE_VALUE;
    if (!::CloseHandle(file) && !error_) error_ = ::GetLastError();
  }

  mutable std::mutex mutex_;
  const std::uint64_t part_bytes_;
  HANDLE file_{INVALID_HANDLE_VALUE};
  DWORD error_{};
  std::filesystem::path directory_;
  wchar_t stem_[64]{};
  unsigned int part_{1};
  std::uint64_t written_{};
  std::uint64_t header_bytes_{};
  std::uint64_t page_window_{};
  unsigned int page_count_{};
  std::uint64_t page_dropped_{};
};

// The file every runtime event goes to. The owner publishes it once the file
// is open and withdraws it before the file goes away.
inline std::atomic<DebugLogFile*>& runtime_log_slot() noexcept {
  static std::atomic<DebugLogFile*> slot{nullptr};
  return slot;
}
inline void set_runtime_log(DebugLogFile* const file) noexcept {
  runtime_log_slot().store(file);
}

// Converts a wide string for the UTF-8 log. Unrepresentable input yields a
// marker instead of a partial string.
[[nodiscard]] inline std::string utf8(const std::wstring_view text) noexcept {
  try {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "<无法转换的文本>";
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
  } catch (...) {
    return "<无法转换的文本>";
  }
}

// Exception text from the C++ runtime and from system_category comes in the
// active code page. Text that is not already UTF-8 is converted from it.
[[nodiscard]] inline std::string as_utf8(const std::string_view text) noexcept {
  try {
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
            nullptr, 0) > 0) {
      return std::string(text);
    }
    const int wide = ::MultiByteToWideChar(CP_ACP, 0, text.data(), length, nullptr, 0);
    if (wide <= 0) return "<无法转换的文本>";
    std::wstring converted(static_cast<size_t>(wide), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.data(), length, converted.data(), wide);
    return utf8(converted);
  } catch (...) {
    return "<无法转换的文本>";
  }
}

// Type, error category, numeric code and text of one exception.
[[nodiscard]] inline std::string describe_exception(const std::exception_ptr& failure) noexcept {
  if (!failure) return "异常：无";
  try {
    try {
      std::rethrow_exception(failure);
    } catch (const std::system_error& error) {
      char code[64]{};
      std::snprintf(code, sizeof(code), "%d（0x%08X）", error.code().value(),
                    static_cast<unsigned int>(error.code().value()));
      return std::string("异常类型：") + typeid(error).name() + "；错误类别：" +
             error.code().category().name() + "；错误码：" + code + "；原文：" +
             as_utf8(error.what());
    } catch (const std::bad_alloc&) {
      return "异常类型：std::bad_alloc；原文：内存不足";
    } catch (const std::exception& error) {
      return std::string("异常类型：") + typeid(error).name() + "；原文：" + as_utf8(error.what());
    } catch (...) {
      return "异常类型：未知（非 std::exception）";
    }
  } catch (...) {
    return "异常：无法描述";
  }
}

#if USB_VAULT_DEBUG_LOG

// One runtime event: local time to the millisecond, thread, area, event and
// optional detail. Never throws; drops the line if it cannot be formed.
inline void debug_log(const std::string_view area, const std::string_view event,
                      const std::string_view detail = {}) noexcept {
  DebugLogFile* const file = runtime_log_slot().load();
  if (file == nullptr) return;
  try {
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    char prefix[96]{};
    std::snprintf(prefix, sizeof(prefix),
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [线程 %lu] [",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, static_cast<unsigned long>(::GetCurrentThreadId()));
    std::string line(prefix);
    line.append(area).append("] ").append(event);
    if (!detail.empty()) line.append("；").append(detail);
    line.append("\r\n");
    file->write(line);
  } catch (...) {
  }
}

// Logs the exception being handled. Call it only inside a catch block.
inline void debug_log_exception(const std::string_view area, const std::string_view event,
                                const std::string_view detail = {}) noexcept {
  try {
    std::string text(detail);
    if (!text.empty()) text.append("；");
    text.append(describe_exception(std::current_exception()));
    debug_log(area, event, text);
  } catch (...) {
  }
}

#else

inline void debug_log(std::string_view, std::string_view, std::string_view = {}) noexcept {}
inline void debug_log_exception(std::string_view, std::string_view, std::string_view = {}) noexcept {}

#endif

// Small shared formatters for the runtime log details.
[[nodiscard]] inline std::string flag_text(const bool value) { return value ? "是" : "否"; }
[[nodiscard]] inline std::string hex_text(const std::uint32_t value) {
  char text[16]{};
  std::snprintf(text, sizeof(text), "0x%08X", value);
  return text;
}
[[nodiscard]] inline std::string drive_text(const wchar_t drive) {
  const wchar_t upper = drive >= L'a' && drive <= L'z' ? static_cast<wchar_t>(drive - L'a' + L'A') : drive;
  if (upper < L'A' || upper > L'Z') return "无";
  return std::string(1, static_cast<char>(upper)) + ":";
}

}  // namespace usb_vault::diagnostics

// Records one runtime event. The detail expression is evaluated only when the
// switch is on, and nothing it throws can leave the statement. The calling
// thread's last Win32 error is kept, so a trace placed before code that reads
// it changes nothing.
#define USB_VAULT_TRACE(area, event, detail)                                  \
  do {                                                                        \
    if constexpr (USB_VAULT_DEBUG_LOG) {                                      \
      const DWORD usb_vault_trace_last_error = ::GetLastError();              \
      try {                                                                   \
        ::usb_vault::diagnostics::debug_log((area), (event), (detail));       \
      } catch (...) {                                                         \
      }                                                                       \
      ::SetLastError(usb_vault_trace_last_error);                             \
    }                                                                         \
  } while (false)
// The same inside a catch block, adding the exception being handled.
#define USB_VAULT_TRACE_FAILURE(area, event, detail)                          \
  do {                                                                        \
    if constexpr (USB_VAULT_DEBUG_LOG) {                                      \
      const DWORD usb_vault_trace_last_error = ::GetLastError();              \
      try {                                                                   \
        ::usb_vault::diagnostics::debug_log_exception((area), (event), (detail)); \
      } catch (...) {                                                         \
      }                                                                       \
      ::SetLastError(usb_vault_trace_last_error);                             \
    }                                                                         \
  } while (false)

namespace usb_vault::diagnostics {

}  // namespace usb_vault::diagnostics
