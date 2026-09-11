#include "usb_vault/volume_creation/volume_creator.h"

#include "volume_creation/creator_protocol.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace usb_vault::volume_creation {
namespace {

constexpr std::chrono::seconds kHelperConnectionTimeout{60};
// How long the caller waits for the helper to be gone before going on to
// remove the incomplete container. The helper holds the container file open
// through the format backend, so deleting it while the helper is still alive
// fails with a sharing violation; waiting here is what turns a cancel into a
// clean one. It is a bound, not an expectation: a helper that outstays it
// leaves the delete to fail loudly rather than hanging the session forever.
constexpr std::chrono::seconds kHelperExitTimeout{30};

class CreationErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "usb-vault-volume-creation";
  }

  [[nodiscard]] std::string message(const int value) const override {
    switch (static_cast<CreationError>(value)) {
      case CreationError::invalid_request:
        return "invalid volume-creation request";
      case CreationError::protocol_error:
        return "invalid volume-creation IPC message";
      case CreationError::elevation_cancelled:
        return "volume creation elevation was cancelled";
      case CreationError::helper_launch_failed:
        return "volume-creation helper could not be launched";
      case CreationError::helper_connection_failed:
        return "volume-creation helper IPC connection failed";
      case CreationError::helper_not_elevated:
        return "volume-creation helper is not elevated";
      case CreationError::format_backend_unavailable:
        return "VeraCrypt format backend is unavailable";
      case CreationError::format_initialization_failed:
        return "VeraCrypt format backend initialization failed";
      case CreationError::format_rejected:
        return "VeraCrypt rejected the volume format operation";
      case CreationError::target_exists:
        return "target container already exists";
      case CreationError::target_access_denied:
        return "target container is not writable by the calling user";
      case CreationError::mount_failed:
        return "temporary outer-volume mount failed";
      case CreationError::bitmap_scan_failed:
        return "filesystem allocation bitmap scan failed";
      case CreationError::insufficient_tail_space:
        return "not enough tail-aligned free space for a hidden volume";
      case CreationError::cleanup_failed:
        return "temporary volume cleanup failed";
      case CreationError::operation_cancelled:
        return "volume creation was cancelled";
      case CreationError::io_error:
        return "volume-creation I/O failed";
      case CreationError::helper_terminated:
        return "volume-creation helper terminated unexpectedly";
      default:
        return "unknown volume-creation error";
    }
  }
};

// Waits for the helper to be gone. Declared before the pipe at the call site
// so the pipe is closed first: a helper still inside the format backend
// notices the closed pipe on its next progress tick and leaves.
class HelperExitWait final {
 public:
  explicit HelperExitWait(const HANDLE process) noexcept : process_(process) {}
  ~HelperExitWait() noexcept {
    if (process_ != nullptr && process_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::WaitForSingleObject(
          process_,
          static_cast<DWORD>(std::chrono::milliseconds(kHelperExitTimeout)
                                 .count())));
    }
  }

  HelperExitWait(const HelperExitWait&) = delete;
  HelperExitWait& operator=(const HelperExitWait&) = delete;

 private:
  HANDLE process_;
};

class Handle final {
 public:
  Handle() noexcept = default;
  explicit Handle(const HANDLE handle) noexcept : handle_(handle) {}
  ~Handle() noexcept { reset(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  Handle(Handle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  void reset() noexcept {
    if (*this) {
      static_cast<void>(::CloseHandle(handle_));
    }
    handle_ = INVALID_HANDLE_VALUE;
  }

  HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[noreturn]] void throw_creation(const CreationError error,
                                 const char* const context) {
  throw std::system_error(make_error_code(error), context);
}

[[nodiscard]] bool known_error(const std::int32_t value) noexcept {
  return value >= static_cast<std::int32_t>(CreationError::invalid_request) &&
         value <= static_cast<std::int32_t>(CreationError::helper_terminated);
}

[[nodiscard]] std::wstring hex_nonce(
    const std::array<std::byte, detail::kNonceSize>& nonce) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(nonce.size() * 2);
  for (const std::byte value : nonce) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(digits[(byte >> 4U) & 0x0fU]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

[[nodiscard]] std::array<std::byte, detail::kNonceSize> random_nonce() {
  std::array<std::byte, detail::kNonceSize> nonce{};
  const NTSTATUS status = ::BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(nonce.data()),
      static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            "generate volume-creation IPC nonce");
  }
  return nonce;
}

[[nodiscard]] std::filesystem::path validated_helper_path(
    const std::filesystem::path& path) {
  const std::filesystem::path normalized =
      std::filesystem::absolute(path).lexically_normal();
  const DWORD attributes = ::GetFileAttributesW(normalized.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    throw std::invalid_argument(
        "volume-creation helper must be an existing non-reparse file");
  }
  return normalized;
}

[[nodiscard]] std::wstring validated_target_path(
    const std::wstring_view target_path) {
  if (target_path.empty() ||
      target_path.size() > detail::kMaxPathCharacters ||
      target_path.find(L'\0') != std::wstring_view::npos) {
    throw std::invalid_argument("invalid volume container path");
  }
  const std::filesystem::path input(target_path);
  const std::filesystem::path normalized = input.lexically_normal();
  if (!input.is_absolute() || normalized.root_name().empty() ||
      normalized.filename().empty() || normalized.native().starts_with(L"\\\\")) {
    throw std::invalid_argument(
        "volume container path must be an absolute local file path");
  }
  const std::wstring value = normalized.native();
  const std::size_t extra_colon = value.find(L':', 2);
  if (extra_colon != std::wstring::npos) {
    throw std::invalid_argument("alternate data stream paths are not allowed");
  }
  return value;
}

[[nodiscard]] Handle launch_elevated_helper(
    const std::filesystem::path& helper_path, const std::wstring& pipe_suffix,
    const std::wstring& nonce_hex) {
  const std::wstring parameters =
      std::format(L"--pipe {} --nonce {}", pipe_suffix, nonce_hex);
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
               SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"runas";
  info.lpFile = helper_path.c_str();
  info.lpParameters = parameters.c_str();
  info.nShow = SW_HIDE;
  if (::ShellExecuteExW(&info) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_CANCELLED) {
      throw_creation(CreationError::elevation_cancelled,
                     "launch elevated volume-creation helper");
    }
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "launch elevated volume-creation helper");
  }
  if (info.hProcess == nullptr) {
    throw_creation(CreationError::helper_launch_failed,
                   "volume-creation helper returned no process handle");
  }
  return Handle(info.hProcess);
}

[[nodiscard]] Handle connect_to_helper(const std::wstring& pipe_name,
                                       const HANDLE process) {
  const auto deadline =
      std::chrono::steady_clock::now() + kHelperConnectionTimeout;
  for (;;) {
    const HANDLE pipe = ::CreateFileW(
        pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      return Handle(pipe);
    }
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "connect to volume-creation helper");
    }
    if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
      throw_creation(CreationError::helper_terminated,
                     "volume-creation helper exited before IPC connection");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw_creation(CreationError::helper_connection_failed,
                     "timed out connecting to volume-creation helper");
    }
    static_cast<void>(::WaitNamedPipeW(pipe_name.c_str(), 100));
  }
}

void invoke(const std::filesystem::path& helper_path,
            const detail::Operation operation, const std::wstring_view path,
            const std::uint64_t outer_size,
            const std::uint64_t hidden_size,
            const security::SecureBuffer& first_password,
            const security::SecureBuffer& second_password,
            const ProgressCallback& progress,
            const CancellationQuery& cancelled) {
  const std::wstring normalized_path = validated_target_path(path);
  if (outer_size == 0 || outer_size % 512ULL != 0 ||
      first_password.empty() ||
      first_password.size() > detail::kMaxPasswordBytes ||
      second_password.size() > detail::kMaxPasswordBytes ||
      (operation == detail::Operation::create_standard &&
       (hidden_size != 0 || !second_password.empty())) ||
      (operation == detail::Operation::create_with_hidden &&
       (second_password.empty() || hidden_size % 512ULL != 0))) {
    throw std::invalid_argument("invalid volume-creation request values");
  }
  const auto nonce = random_nonce();
  const std::wstring suffix = hex_nonce(nonce);
  const std::wstring pipe_name = L"\\\\.\\pipe\\usb-vault-create-" + suffix;
  const Handle process =
      launch_elevated_helper(helper_path, suffix, hex_nonce(nonce));
  const HelperExitWait helper_exit(process.get());
  const Handle pipe = connect_to_helper(pipe_name, process.get());
  if (progress) progress({.stage = CreationStage::authorized, .percent = 100});

  detail::RequestHeader header{
      .operation = operation,
      .outer_size_bytes = outer_size,
      .hidden_size_bytes = hidden_size,
      .path_characters = static_cast<std::uint32_t>(normalized_path.size()),
      .first_password_bytes =
          static_cast<std::uint32_t>(first_password.size()),
      .second_password_bytes =
          static_cast<std::uint32_t>(second_password.size()),
      .nonce = nonce,
  };
  detail::write_request(pipe.get(), header, normalized_path, first_password,
                        second_password);

  bool cancel_sent = false;
  for (;;) {
    const detail::Response response = detail::read_response(pipe.get());
    if (response.type == detail::MessageType::progress) {
      if (progress) {
        progress({.stage = response.stage, .percent = response.percent});
      }
      // Asked after the report rather than before it, so the page has seen
      // where the creation got to before it is told it stopped. The request
      // goes out once and the loop keeps reading: the helper answers it with
      // a result of its own, after it has removed the container it started.
      if (!cancel_sent && cancelled && cancelled()) {
        detail::write_cancel_request(pipe.get(), nonce);
        cancel_sent = true;
      }
      continue;
    }
    if (response.error == 0) {
      return;
    }
    if (!known_error(response.error)) {
      throw_creation(CreationError::protocol_error,
                     "helper returned an unknown error code");
    }
    throw_creation(static_cast<CreationError>(response.error),
                   "volume-creation helper failed");
  }
}

}  // namespace

const std::error_category& creation_error_category() noexcept {
  static CreationErrorCategory category;
  return category;
}

std::error_code make_error_code(const CreationError error) noexcept {
  return {static_cast<int>(error), creation_error_category()};
}

VolumeCreator::VolumeCreator(std::filesystem::path helper_path)
    : helper_path_(validated_helper_path(helper_path)) {}

void VolumeCreator::create_standard(
    const std::wstring_view target_path, const std::uint64_t size_bytes,
    const security::SecureBuffer& volume_password,
    const ProgressCallback& progress,
    const CancellationQuery& cancelled) const {
  invoke(helper_path_, detail::Operation::create_standard, target_path,
         size_bytes, 0, volume_password, security::SecureBuffer{}, progress,
         cancelled);
}

void VolumeCreator::create_with_hidden(
    const std::wstring_view target_path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t requested_hidden_bytes,
    const security::SecureBuffer& outer_password,
    const security::SecureBuffer& hidden_password,
    const ProgressCallback& progress,
    const CancellationQuery& cancelled) const {
  invoke(helper_path_, detail::Operation::create_with_hidden, target_path,
         outer_size_bytes, requested_hidden_bytes, outer_password,
         hidden_password, progress, cancelled);
}

}  // namespace usb_vault::volume_creation
