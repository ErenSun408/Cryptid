#include "volume_creation/creator_helper_engine.h"
#include "volume_creation/creator_pipe_security.h"
#include "volume_creation/creator_protocol.h"
#include "volume_creation/elevation_session.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <Windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using usb_vault::volume_creation::CreationError;
using usb_vault::volume_creation::creation_error_category;
using usb_vault::volume_creation::detail::Request;
using usb_vault::volume_creation::detail::Response;

class LocalMemory final {
 public:
  explicit LocalMemory(HLOCAL value = nullptr) noexcept : value_(value) {}
  ~LocalMemory() noexcept {
    if (value_ != nullptr) {
      static_cast<void>(::LocalFree(value_));
    }
  }
  LocalMemory(const LocalMemory&) = delete;
  LocalMemory& operator=(const LocalMemory&) = delete;
  [[nodiscard]] HLOCAL get() const noexcept { return value_; }

 private:
  HLOCAL value_;
};

class Handle final {
 public:
  explicit Handle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
      : value_(value) {}
  ~Handle() noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(value_));
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return value_; }

 private:
  HANDLE value_;
};

[[nodiscard]] bool is_elevated() noexcept {
  HANDLE raw_token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) ==
      FALSE) {
    return false;
  }
  const Handle token(raw_token);
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  return ::GetTokenInformation(token.get(), TokenElevation, &elevation,
                               sizeof(elevation), &returned) != FALSE &&
         elevation.TokenIsElevated != 0;
}

[[nodiscard]] int hex_value(const wchar_t value) noexcept {
  if (value >= L'0' && value <= L'9') return value - L'0';
  if (value >= L'a' && value <= L'f') return value - L'a' + 10;
  if (value >= L'A' && value <= L'F') return value - L'A' + 10;
  return -1;
}

[[nodiscard]] bool parse_nonce(
    const std::wstring_view text,
    std::array<std::byte,
               usb_vault::volume_creation::detail::kNonceSize>& result) {
  if (text.size() != result.size() * 2) return false;
  for (std::size_t index = 0; index < result.size(); ++index) {
    const int high = hex_value(text[index * 2]);
    const int low = hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    result[index] = static_cast<std::byte>((high << 4) | low);
  }
  return true;
}

struct Arguments final {
  std::wstring pipe_suffix;
  std::array<std::byte,
             usb_vault::volume_creation::detail::kNonceSize> nonce{};
};

[[nodiscard]] bool parse_arguments(const int count, wchar_t** const values,
                                   Arguments& result) {
  if (count != 5 || std::wstring_view(values[1]) != L"--pipe" ||
      std::wstring_view(values[3]) != L"--nonce") {
    return false;
  }
  const std::wstring_view suffix(values[2]);
  if (suffix.size() !=
      usb_vault::volume_creation::detail::kNonceSize * 2) {
    return false;
  }
  for (const wchar_t value : suffix) {
    if (hex_value(value) < 0) return false;
  }
  result.pipe_suffix.assign(suffix);
  return parse_nonce(values[4], result.nonce) &&
         suffix == std::wstring_view(values[4]);
}

void send_failure(const HANDLE pipe, const CreationError error,
                  const DWORD native_error = 0) noexcept {
  USB_VAULT_TRACE("创建助手进程", "回传失败",
                  "错误码 " + std::to_string(static_cast<int>(error)) + "；原生码 " +
                      std::to_string(native_error) + "（" +
                      usb_vault::diagnostics::hex_text(native_error) + "）");
  try {
    usb_vault::volume_creation::detail::write_response(
        pipe, Response{
                  .type = usb_vault::volume_creation::detail::MessageType::result,
                  .error = static_cast<std::int32_t>(error),
                  .native_error = native_error,
              });
  } catch (...) {
  }
}

// The step each early exit stood at, with the Win32 error that ended it.
[[nodiscard]] int stopped(const int code, const char* const step) noexcept {
  const DWORD error = ::GetLastError();
  USB_VAULT_TRACE("创建助手进程", "提前退出",
                  std::string(step) + "；退出码 " + std::to_string(code) + "；Win32 " +
                      std::to_string(error));
  return code;
}

[[nodiscard]] std::filesystem::path helper_directory() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

int run_helper();

}  // namespace

// The helper keeps a log file of its own beside the application's, named by
// its process id, so the codes the protocol reduces to one number survive.
int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                     _In_ int) {
  usb_vault::diagnostics::DebugLogFile log;
  if constexpr (USB_VAULT_DEBUG_LOG) {
    try {
      const auto directory = helper_directory();
      SYSTEMTIME time{};
      ::GetLocalTime(&time);
      if (!directory.empty() &&
          log.open(directory, time, L"creator-log-" + std::to_wstring(::GetCurrentProcessId()))) {
        usb_vault::diagnostics::set_runtime_log(&log);
      }
    } catch (...) {
    }
  }
  USB_VAULT_TRACE("创建助手进程", "启动",
                  "进程号 " + std::to_string(::GetCurrentProcessId()) + "；命令行 " +
                      usb_vault::diagnostics::utf8(::GetCommandLineW()));
  int code = 5;
  try {
    code = run_helper();
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("创建助手进程", "未处理的异常", "");
  }
  USB_VAULT_TRACE("创建助手进程", "退出", "退出码 " + std::to_string(code));
  usb_vault::diagnostics::set_runtime_log(nullptr);
  return code;
}

namespace {

int run_helper() {
  int count = 0;
  const LocalMemory arguments_memory(
      reinterpret_cast<HLOCAL>(::CommandLineToArgvW(::GetCommandLineW(),
                                                     &count)));
  if (arguments_memory.get() == nullptr) return stopped(2, "解析命令行");
  auto** values = reinterpret_cast<wchar_t**>(arguments_memory.get());
  DWORD owner_pid{};
  if (count == 7 && (std::wstring_view(values[1]) == L"--session" || std::wstring_view(values[1]) == L"--owner")) {
    DWORD parent{};
    const std::wstring_view text(values[2]);
    if (text.empty() || text.size() > 10) return stopped(2, "父进程号格式");
    for (auto c : text) {
      if (c < L'0' || c > L'9' || parent > (MAXDWORD - static_cast<DWORD>(c-L'0')) / 10) return stopped(2, "父进程号格式");
      parent = parent * 10 + static_cast<DWORD>(c-L'0');
    }
    if (!parent) return stopped(2, "父进程号为 0");
    Arguments session;
    if (!parse_arguments(count-2, values+2, session)) return stopped(2, "会话参数");
    if (std::wstring_view(values[1]) == L"--session") {
      USB_VAULT_TRACE("创建助手进程", "作为管理员会话宿主运行", "父进程 " + std::to_string(parent));
      return usb_vault::volume_creation::detail::run_elevation_session(parent, session.pipe_suffix);
    }
    owner_pid=parent; count-=2; values+=2;
  }
  Arguments arguments;
  if (!parse_arguments(count, values, arguments)) return stopped(2, "管道参数");
  USB_VAULT_TRACE("创建助手进程", owner_pid ? "由管理员会话启动" : "由 UAC 直接启动",
                  "所属进程 " + std::to_string(owner_pid));

  const Handle owner(owner_pid ? ::OpenProcess(SYNCHRONIZE, FALSE, owner_pid) : nullptr);
  if (owner_pid && !owner.get()) return stopped(3, "打开所属进程");
  const std::wstring pipe_name =
      L"\\\\.\\pipe\\usb-vault-create-" + arguments.pipe_suffix;
  auto descriptor = usb_vault::volume_creation::detail::
      current_user_pipe_security_descriptor();
  if (descriptor.get() == nullptr) return stopped(3, "构造管道安全描述符");
  SECURITY_ATTRIBUTES security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = descriptor.get(),
      .bInheritHandle = FALSE,
  };
  const Handle pipe(::CreateNamedPipeW(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | (owner_pid ? PIPE_NOWAIT : PIPE_WAIT) |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, &security));
  if (pipe.get() == INVALID_HANDLE_VALUE) return stopped(3, "创建管道");
  if (owner_pid) {
    const auto start=::GetTickCount64(); bool connected=false;
    while (::WaitForSingleObject(owner.get(),0)==WAIT_TIMEOUT && ::GetTickCount64()-start<60000) {
      if (::ConnectNamedPipe(pipe.get(),nullptr) || ::GetLastError()==ERROR_PIPE_CONNECTED) {connected=true;break;}
      if (::GetLastError()!=ERROR_PIPE_LISTENING) return stopped(3, "等待调用方连接");
      ::Sleep(20);
    }
    ULONG client{}; DWORD mode=PIPE_READMODE_BYTE|PIPE_WAIT;
    if (!connected || !::GetNamedPipeClientProcessId(pipe.get(),&client) || client!=owner_pid ||
        !::SetNamedPipeHandleState(pipe.get(),&mode,nullptr,nullptr))
      return stopped(3, connected ? "核对调用方进程" : "调用方未在期限内连接");
  } else if (::ConnectNamedPipe(pipe.get(), nullptr) == FALSE &&
      ::GetLastError() != ERROR_PIPE_CONNECTED) {
    return stopped(3, "等待调用方连接");
  }
  USB_VAULT_TRACE("创建助手进程", "调用方已连接", "");

  if (!is_elevated()) {
    send_failure(pipe.get(), CreationError::helper_not_elevated);
    return stopped(4, "进程未提权");
  }

  try {
    Request request = usb_vault::volume_creation::detail::read_request(
        pipe.get(), arguments.nonce);
    USB_VAULT_TRACE("创建助手进程", "收到请求",
                    "操作 " + std::to_string(static_cast<int>(request.operation)) + "；路径 " +
                        usb_vault::diagnostics::utf8(request.target_path) + "；外层 " +
                        std::to_string(request.outer_size_bytes) + " 字节；隐藏卷 " +
                        std::to_string(request.hidden_size_bytes) + " 字节；盘符序号 " +
                        std::to_string(request.mounted_drive) + "；挂载号 " +
                        std::to_string(request.mount_id) + "；来源 " +
                        usb_vault::diagnostics::utf8(request.source_path) + "；目标 " +
                        usb_vault::diagnostics::utf8(request.destination_relative) +
                        "；第一口令长度 " + std::to_string(request.first_password.size()) +
                        "；第二口令长度 " + std::to_string(request.second_password.size()));
    if (request.operation ==
        usb_vault::volume_creation::detail::Operation::scan_tail_space ||
        request.operation == usb_vault::volume_creation::detail::Operation::scan_protected_space) {
      // One consent, one run of measurements: the caller keeps the pipe while
      // its maintenance session lasts and this process ends when it lets go.
      usb_vault::volume_creation::detail::serve_measurement_session(
          pipe.get(), arguments.nonce, request);
      static_cast<void>(::FlushFileBuffers(pipe.get()));
      return 0;
    }
    if (request.operation == usb_vault::volume_creation::detail::Operation::scan_maintenance_space) {
      usb_vault::volume_creation::detail::serve_maintenance_session(pipe.get(), arguments.nonce, request);
      static_cast<void>(::FlushFileBuffers(pipe.get()));
      return 0;
    }
    if (request.operation == usb_vault::volume_creation::detail::Operation::import_protected_file) {
      usb_vault::volume_creation::detail::serve_import_session(pipe.get(), arguments.nonce, request);
      static_cast<void>(::FlushFileBuffers(pipe.get()));
      return 0;
    }
    const std::uint64_t value =
        usb_vault::volume_creation::detail::execute_request(pipe.get(),
                                                            request);
    request.first_password.clear();
    request.second_password.clear();
    USB_VAULT_TRACE("创建助手进程", "请求完成", "返回值 " + std::to_string(value));
    usb_vault::volume_creation::detail::write_response(
        pipe.get(),
        Response{.type =
                     usb_vault::volume_creation::detail::MessageType::result,
                 .value = value});
    static_cast<void>(::FlushFileBuffers(pipe.get()));
    return 0;
  } catch (const std::system_error& error) {
    USB_VAULT_TRACE_FAILURE("创建助手进程", "请求失败", "");
    if (error.code().category() == creation_error_category()) {
      send_failure(pipe.get(),
                   static_cast<CreationError>(error.code().value()));
    } else {
      send_failure(pipe.get(), CreationError::io_error,
                   static_cast<DWORD>(error.code().value()));
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("创建助手进程", "请求失败", "");
    send_failure(pipe.get(), CreationError::io_error);
  }
  return 5;
}

}  // namespace
