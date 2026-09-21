#include "volume_creation/creator_helper_engine.h"
#include "volume_creation/creator_pipe_security.h"
#include "volume_creation/creator_protocol.h"

#include <Windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
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

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                     _In_ int) {
  int count = 0;
  const LocalMemory arguments_memory(
      reinterpret_cast<HLOCAL>(::CommandLineToArgvW(::GetCommandLineW(),
                                                     &count)));
  if (arguments_memory.get() == nullptr) return 2;
  auto** values = reinterpret_cast<wchar_t**>(arguments_memory.get());
  Arguments arguments;
  if (!parse_arguments(count, values, arguments)) return 2;

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\usb-vault-create-" + arguments.pipe_suffix;
  auto descriptor = usb_vault::volume_creation::detail::
      current_user_pipe_security_descriptor();
  if (descriptor.get() == nullptr) return 3;
  SECURITY_ATTRIBUTES security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = descriptor.get(),
      .bInheritHandle = FALSE,
  };
  const Handle pipe(::CreateNamedPipeW(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, &security));
  if (pipe.get() == INVALID_HANDLE_VALUE) return 3;
  if (::ConnectNamedPipe(pipe.get(), nullptr) == FALSE &&
      ::GetLastError() != ERROR_PIPE_CONNECTED) {
    return 3;
  }

  if (!is_elevated()) {
    send_failure(pipe.get(), CreationError::helper_not_elevated);
    return 4;
  }

  try {
    Request request = usb_vault::volume_creation::detail::read_request(
        pipe.get(), arguments.nonce);
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
    usb_vault::volume_creation::detail::write_response(
        pipe.get(),
        Response{.type =
                     usb_vault::volume_creation::detail::MessageType::result,
                 .value = value});
    static_cast<void>(::FlushFileBuffers(pipe.get()));
    return 0;
  } catch (const std::system_error& error) {
    if (error.code().category() == creation_error_category()) {
      send_failure(pipe.get(),
                   static_cast<CreationError>(error.code().value()));
    } else {
      send_failure(pipe.get(), CreationError::io_error,
                   static_cast<DWORD>(error.code().value()));
    }
  } catch (...) {
    send_failure(pipe.get(), CreationError::io_error);
  }
  return 5;
}
