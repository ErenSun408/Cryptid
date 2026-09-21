#include "usb_vault/volume_creation/volume_creator.h"

#include "volume_creation/creator_protocol.h"
#include "usb_vault/volume/volume_entry_path.h"

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

[[nodiscard]] std::uint64_t invoke(
    const std::filesystem::path& helper_path,
    const detail::Operation operation, const std::wstring_view path,
    const std::uint64_t outer_size,
    const std::uint64_t hidden_size,
    const security::SecureBuffer& first_password,
    const security::SecureBuffer& second_password,
    const ProgressCallback& progress,
    const CancellationQuery& cancelled) {
  const bool measuring = operation == detail::Operation::scan_tail_space;
  // A measurement names a volume root, which is not a container path and does
  // not pass the checks meant for one.
  const std::wstring normalized_path =
      measuring ? std::wstring(path) : validated_target_path(path);
  if (measuring) {
    if (outer_size != 0 || hidden_size != 0 || !first_password.empty() ||
        !second_password.empty()) {
      throw std::invalid_argument("invalid volume-measurement request values");
    }
  } else if (outer_size == 0 || outer_size % 512ULL != 0 ||
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
      return response.value;
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

[[nodiscard]] wchar_t validated_drive_letter(const wchar_t drive_letter) {
  const wchar_t letter = (drive_letter >= L'a' && drive_letter <= L'z')
                             ? static_cast<wchar_t>(drive_letter - L'a' + L'A')
                             : drive_letter;
  if (letter < L'A' || letter > L'Z') {
    throw std::invalid_argument("drive letter must be in the range A-Z");
  }
  return letter;
}

struct VolumeImportSession::Impl final {
  std::filesystem::path helper_path;
  std::array<std::byte, detail::kNonceSize> nonce{};
  Handle process, pipe;
  std::wstring container;
  wchar_t drive{};
  std::int32_t mount_id{};
  std::uint64_t mounted_bytes{};
  bool connected{};
  bool failed{};
};
VolumeImportSession::VolumeImportSession(std::filesystem::path helper_path) : impl_(std::make_unique<Impl>()) {
  impl_->helper_path=validated_helper_path(helper_path);
}
VolumeImportSession::~VolumeImportSession() noexcept { close(); }
bool VolumeImportSession::open() const noexcept { return impl_->connected; }
void VolumeImportSession::close() noexcept {
  impl_->failed=false;
  impl_->connected=false;
  impl_->pipe=Handle{};
  if (impl_->process) static_cast<void>(::WaitForSingleObject(impl_->process.get(), 5000));
  impl_->process=Handle{};
  impl_->container.clear();
}
std::uint64_t VolumeImportSession::protected_space(std::wstring_view container_path,
    wchar_t drive, std::int32_t mount_id, std::uint64_t mounted_bytes,
    const security::SecureBuffer& outer_password, const security::SecureBuffer& hidden_password) {
  if (impl_->failed) throw std::logic_error("maintenance session requires recovery");
  try {
    const auto container = validated_target_path(container_path);
    const auto letter = validated_drive_letter(drive);
    detail::RequestHeader header{.operation = detail::Operation::scan_maintenance_space,
        .outer_size_bytes = mounted_bytes,
        .path_characters = static_cast<std::uint32_t>(container.size()),
        .first_password_bytes = static_cast<std::uint32_t>(outer_password.size()),
        .second_password_bytes = static_cast<std::uint32_t>(hidden_password.size()),
        .mounted_drive = static_cast<std::uint32_t>(letter - L'A' + 1),
        .mount_id = mount_id, .nonce = impl_->nonce};
    if (!detail::valid_request_header(header)) throw std::invalid_argument("invalid maintenance measurement");
    if (impl_->connected && (impl_->container != container || impl_->drive != letter ||
        impl_->mount_id != mount_id || impl_->mounted_bytes != mounted_bytes))
      throw std::logic_error("maintenance session cannot change mounts");
    if (!impl_->connected) {
      impl_->nonce = random_nonce(); header.nonce = impl_->nonce;
      const auto suffix = hex_nonce(impl_->nonce);
      impl_->process = launch_elevated_helper(impl_->helper_path, suffix, suffix);
    impl_->pipe = connect_to_helper(L"\\\\.\\pipe\\usb-vault-create-" + suffix, impl_->process.get());
      impl_->container = container; impl_->drive = letter;
      impl_->mount_id = mount_id; impl_->mounted_bytes = mounted_bytes;
      impl_->connected = true;
    }
    detail::write_request(impl_->pipe.get(), header, container, outer_password, hidden_password);
    for (;;) {
      const auto response = detail::read_response(impl_->pipe.get());
      if (response.type == detail::MessageType::progress) continue;
      if (response.error == 0) return response.value;
      if (!known_error(response.error)) throw_creation(CreationError::protocol_error, "invalid maintenance measurement result");
      throw_creation(static_cast<CreationError>(response.error), "maintenance measurement failed");
    }
  } catch (...) { close(); throw; }
}

void VolumeImportSession::copy(std::wstring_view container_path, wchar_t drive, std::int32_t mount_id,
    std::uint64_t mounted_bytes, const security::SecureBuffer& outer_password,
    const security::SecureBuffer& hidden_password, std::wstring_view source_path,
    std::wstring_view destination_relative, const ProgressCallback& progress,
    const CancellationQuery& cancelled) {
  if (impl_->failed) throw std::logic_error("protected import session requires recovery");
  try {
  const auto container = validated_target_path(container_path);
  const auto source = validated_target_path(source_path);
  const auto letter = validated_drive_letter(drive);
  const auto destination = volume::resolve_volume_entry_path(letter, destination_relative).substr(3);
  if (destination.empty()) throw std::invalid_argument("missing import filename");
  detail::RequestHeader header{.operation = detail::Operation::import_protected_file,
      .outer_size_bytes = mounted_bytes,
      .path_characters = static_cast<std::uint32_t>(container.size()),
      .first_password_bytes = static_cast<std::uint32_t>(outer_password.size()),
      .second_password_bytes = static_cast<std::uint32_t>(hidden_password.size()),
      .mounted_drive = static_cast<std::uint32_t>(letter - L'A' + 1), .mount_id = mount_id,
      .source_characters = static_cast<std::uint32_t>(source.size()),
      .destination_characters = static_cast<std::uint32_t>(destination.size()), .nonce = impl_->nonce};
  if (!detail::valid_request_header(header)) throw std::invalid_argument("invalid protected import");
  if (cancelled && cancelled()) throw_creation(CreationError::operation_cancelled, "protected import cancelled");
  if (impl_->connected && (impl_->container != container || impl_->drive != letter ||
      impl_->mount_id != mount_id || impl_->mounted_bytes != mounted_bytes))
    throw std::logic_error("protected import session cannot change mounts");
  if (!impl_->connected) {
    impl_->nonce = random_nonce(); header.nonce = impl_->nonce;
    const auto suffix = hex_nonce(impl_->nonce);
    impl_->process = launch_elevated_helper(impl_->helper_path, suffix, suffix);
    impl_->pipe = connect_to_helper(L"\\\\.\\pipe\\usb-vault-create-" + suffix, impl_->process.get());
    impl_->container=container;impl_->drive=letter;impl_->mount_id=mount_id;impl_->mounted_bytes=mounted_bytes;
    impl_->connected=true;
  }
  const auto& pipe = impl_->pipe;
  detail::write_request(pipe.get(), header, container, outer_password, hidden_password, source, destination);
  bool sent = false;
  for (;;) {
    const auto response = detail::read_response(pipe.get());
    if (response.type == detail::MessageType::progress) {
      if (progress) progress({.stage=response.stage, .percent=response.percent});
      if (!sent && cancelled && cancelled()) {detail::write_cancel_request(pipe.get(), header.nonce);sent=true;}
      continue;
    }
    if (response.error == 0) return;
    if (!known_error(response.error)) throw_creation(CreationError::protocol_error, "invalid protected import result");
    throw_creation(static_cast<CreationError>(response.error), "protected import failed; maintenance recovery required");
  }
  } catch (...) { close(); impl_->failed=true; throw; }
}

void import_protected_file(const std::filesystem::path& helper_path,
    std::wstring_view container_path, wchar_t drive, std::int32_t mount_id,
    std::uint64_t mounted_bytes, const security::SecureBuffer& outer_password,
    const security::SecureBuffer& hidden_password, std::wstring_view source_path,
    std::wstring_view destination_relative, const ProgressCallback& progress,
    const CancellationQuery& cancelled) {
  VolumeImportSession session(helper_path);
  session.copy(container_path,drive,mount_id,mounted_bytes,outer_password,hidden_password,
               source_path,destination_relative,progress,cancelled);
}

struct VolumeMeasurementSession::Impl final {
  std::filesystem::path helper_path;
  std::array<std::byte, detail::kNonceSize> nonce{};
  Handle process{nullptr};
  Handle pipe{nullptr};
  bool connected{false};
};

VolumeMeasurementSession::VolumeMeasurementSession(
    std::filesystem::path helper_path)
    : impl_(std::make_unique<Impl>()) {
  impl_->helper_path = validated_helper_path(helper_path);
}

VolumeMeasurementSession::~VolumeMeasurementSession() noexcept { close(); }

VolumeMeasurementSession::VolumeMeasurementSession(
    VolumeMeasurementSession&&) noexcept = default;

VolumeMeasurementSession& VolumeMeasurementSession::operator=(
    VolumeMeasurementSession&&) noexcept = default;

bool VolumeMeasurementSession::open() const noexcept {
  return impl_ != nullptr && impl_->connected;
}

void VolumeMeasurementSession::close() noexcept {
  if (impl_ == nullptr) return;
  impl_->connected = false;
  // Letting go of the pipe is the whole protocol for ending this: the helper
  // reads the close and exits.
  impl_->pipe = Handle(nullptr);
  if (impl_->process.get() != nullptr) {
    static_cast<void>(::WaitForSingleObject(impl_->process.get(), 5000));
    impl_->process = Handle(nullptr);
  }
}

std::uint64_t VolumeMeasurementSession::tail_space(
    const wchar_t drive_letter) {
  const wchar_t letter = validated_drive_letter(drive_letter);
  const std::wstring root = {letter, L':', L'\\'};
  return measure(root, 0, 0, 0, security::SecureBuffer{}, security::SecureBuffer{});
}

std::uint64_t VolumeMeasurementSession::protected_space(
    std::wstring_view path, wchar_t drive, std::int32_t mount_id,
    std::uint64_t mounted_bytes, const security::SecureBuffer& first,
    const security::SecureBuffer& second) {
  const auto letter = validated_drive_letter(drive);
  const auto normalized = validated_target_path(path);
  if (mount_id < 0 || mounted_bytes == 0 || first.empty() || second.empty() ||
      first.size() > detail::kMaxPasswordBytes || second.size() > detail::kMaxPasswordBytes)
    throw std::invalid_argument("invalid protected-space request");
  return measure(normalized, static_cast<std::uint32_t>(letter - L'A' + 1),
                 mount_id, mounted_bytes, first, second);
}

std::uint64_t VolumeMeasurementSession::measure(
    std::wstring_view root, std::uint32_t drive, std::int32_t mount_id,
    std::uint64_t mounted_bytes, const security::SecureBuffer& first,
    const security::SecureBuffer& second) {
  if (impl_ == nullptr) {
    throw std::logic_error("this measurement session has been moved from");
  }
  if (!impl_->connected) {
    try {
      impl_->nonce = random_nonce();
      const std::wstring suffix = hex_nonce(impl_->nonce);
      const std::wstring pipe_name =
          L"\\\\.\\pipe\\usb-vault-create-" + suffix;
      impl_->process =
          launch_elevated_helper(impl_->helper_path, suffix, suffix);
      impl_->pipe = connect_to_helper(pipe_name, impl_->process.get());
      impl_->connected = true;
    } catch (...) {
      close();
      throw;
    }
  }

  const detail::RequestHeader header{
      .operation = drive ? detail::Operation::scan_protected_space : detail::Operation::scan_tail_space,
      .outer_size_bytes = mounted_bytes,
      .path_characters = static_cast<std::uint32_t>(root.size()),
      .first_password_bytes = static_cast<std::uint32_t>(first.size()),
      .second_password_bytes = static_cast<std::uint32_t>(second.size()),
      .mounted_drive = drive,
      .mount_id = mount_id,
      .nonce = impl_->nonce,
  };
  detail::Response answer{};
  try {
    detail::write_request(impl_->pipe.get(), header, root,
                          first, second);
    for (;;) {
      answer = detail::read_response(impl_->pipe.get());
      if (answer.type != detail::MessageType::progress) break;
    }
  } catch (...) {
    // Transport and framing failures mean the helper can no longer be trusted.
    close();
    throw;
  }
  if (answer.error == 0) return answer.value;
  if (!known_error(answer.error)) {
    close();
    throw_creation(CreationError::protocol_error,
                   "helper returned an unknown error code");
  }
  // A measurement may fail while the pipe remains sound. The helper keeps
  // serving the run, so the next refresh must not ask for consent again.
  throw_creation(static_cast<CreationError>(answer.error),
                 "volume measurement failed");
}

VolumeCreator::VolumeCreator(std::filesystem::path helper_path)
    : helper_path_(validated_helper_path(helper_path)) {}

void VolumeCreator::create_standard(
    const std::wstring_view target_path, const std::uint64_t size_bytes,
    const security::SecureBuffer& volume_password,
    const ProgressCallback& progress,
    const CancellationQuery& cancelled) const {
  static_cast<void>(invoke(helper_path_, detail::Operation::create_standard,
                           target_path, size_bytes, 0, volume_password,
                           security::SecureBuffer{}, progress, cancelled));
}

void VolumeCreator::create_with_hidden(
    const std::wstring_view target_path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t requested_hidden_bytes,
    const security::SecureBuffer& outer_password,
    const security::SecureBuffer& hidden_password,
    const ProgressCallback& progress,
    const CancellationQuery& cancelled) const {
  static_cast<void>(invoke(helper_path_,
                           detail::Operation::create_with_hidden, target_path,
                           outer_size_bytes, requested_hidden_bytes,
                           outer_password, hidden_password, progress,
                           cancelled));
}

std::uint64_t VolumeCreator::scan_tail_space(const wchar_t drive_letter) const {
  const wchar_t letter = (drive_letter >= L'a' && drive_letter <= L'z')
                             ? static_cast<wchar_t>(drive_letter - L'a' + L'A')
                             : drive_letter;
  if (letter < L'A' || letter > L'Z') {
    throw std::invalid_argument("drive letter must be in the range A-Z");
  }
  const std::wstring root = {letter, L':', L'\\'};
  return invoke(helper_path_, detail::Operation::scan_tail_space, root, 0, 0,
                security::SecureBuffer{}, security::SecureBuffer{}, {}, {});
}

}  // namespace usb_vault::volume_creation
