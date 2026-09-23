#include "volume_creation/creator_protocol.h"

#include "usb_vault/volume/partition_identity.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace usb_vault::volume_creation::detail {
namespace {

[[noreturn]] void throw_last_error(const char* const operation) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), operation);
}

[[noreturn]] void throw_protocol_error(const char* const message) {
  throw std::system_error(make_error_code(CreationError::protocol_error),
                          message);
}

[[nodiscard]] bool valid_operation(const Operation operation) noexcept {
  return operation == Operation::format_partition_pair || operation == Operation::decrypt_partition_bitlocker || operation == Operation::inspect_partition_bitlocker || inplace_operation(operation) || operation == Operation::format_partition || operation == Operation::check_partition || operation == Operation::create_standard ||
         operation == Operation::create_with_hidden ||
         operation == Operation::scan_tail_space ||
         operation == Operation::scan_protected_space || operation == Operation::scan_maintenance_space || operation == Operation::import_protected_file;
}

[[nodiscard]] bool valid_stage(const CreationStage stage) noexcept {
  return stage == CreationStage::partition_ready || stage == CreationStage::formatting ||
         stage == CreationStage::formatting_outer ||
         stage == CreationStage::scanning_tail_space ||
         stage == CreationStage::formatting_hidden ||
         stage == CreationStage::authorized || stage == CreationStage::allocated || stage == CreationStage::copying;
}

}  // namespace

bool valid_partition_target(const PartitionTarget& target) noexcept {
  const volume::PartitionIdentity identity{target.device_id, target.device_flags,
      target.layout, target.sector_bytes, target.offset_bytes, target.length_bytes, target.partition_id};
  return target.reserved == 0 && volume::valid_partition_identity(identity) &&
      target.disk_number != UINT32_MAX && target.partition_number > 0 && target.partition_number != UINT32_MAX;
}

bool valid_partition_path(const std::wstring_view path) noexcept {
  if (path.size() != 49 || !path.starts_with(LR"(\\?\Volume{)") || path.substr(47) != LR"(}\)") return false;
  bool nonzero{};
  for (std::size_t i = 11; i < 47; ++i) {
    if (i == 19 || i == 24 || i == 29 || i == 34) { if (path[i] != L'-') return false; continue; }
    const wchar_t c = path[i];
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    nonzero = nonzero || c != L'0';
  }
  return nonzero;
}

bool valid_request_header(const RequestHeader& header) noexcept {
  if (header.magic != kProtocolMagic ||
      header.version != kProtocolVersion ||
      header.header_size != sizeof(RequestHeader) || header.reserved != 0 ||
      !valid_operation(header.operation) || header.path_characters == 0 ||
      header.path_characters > kMaxPathCharacters) {
    return false;
  }

  if (partition_operation(header.operation)) {
    const bool pair = header.operation == Operation::format_partition_pair;
    return header.partition_target_bytes == sizeof(PartitionTarget) && header.path_characters == 49 &&
        header.outer_size_bytes == 0 && (pair ? valid_partition_hidden_size(1ULL << 40, header.hidden_size_bytes) : header.hidden_size_bytes == 0) && header.first_password_bytes == (header.operation != Operation::check_partition && header.operation != Operation::inspect_partition_bitlocker && header.operation != Operation::decrypt_partition_bitlocker ? 64u : 0u) &&
        header.second_password_bytes == (pair ? 64u : 0u) && header.mounted_drive == 0 && header.mount_id == 0 &&
        header.source_characters == 0 && header.destination_characters == 0;
  }
  if (header.partition_target_bytes != 0) return false;
  if (header.operation == Operation::import_protected_file) {
    if (header.source_characters == 0 || header.source_characters > kMaxPathCharacters ||
        header.destination_characters == 0 || header.destination_characters > 256) return false;
  } else if (header.source_characters != 0 || header.destination_characters != 0) return false;
  if (header.operation == Operation::scan_protected_space || header.operation == Operation::scan_maintenance_space || header.operation == Operation::import_protected_file) {
    return header.mounted_drive >= 1 && header.mounted_drive <= 26 &&
           header.mount_id >= 0 && header.outer_size_bytes > 0 &&
           header.hidden_size_bytes == 0 && header.first_password_bytes > 0 &&
           header.first_password_bytes <= kMaxPasswordBytes &&
           header.second_password_bytes > 0 &&
           header.second_password_bytes <= kMaxPasswordBytes;
  }
  if (header.mounted_drive != 0 || header.mount_id != 0) return false;

  // A measurement carries neither a size nor a password, so a request that
  // brings one is not a measurement however it is labelled.
  if (header.operation == Operation::scan_tail_space) {
    return header.outer_size_bytes == 0 && header.hidden_size_bytes == 0 &&
           header.first_password_bytes == 0 &&
           header.second_password_bytes == 0;
  }

  if (header.outer_size_bytes == 0 || header.first_password_bytes == 0 ||
      header.first_password_bytes > kMaxPasswordBytes ||
      header.second_password_bytes > kMaxPasswordBytes) {
    return false;
  }

  if (header.operation == Operation::create_standard) {
    return header.hidden_size_bytes == 0 &&
           header.second_password_bytes == 0;
  }
  return header.second_password_bytes != 0;
}

bool valid_response(const Response& response) noexcept {
  if (response.magic != kProtocolMagic ||
      response.version != kProtocolVersion || response.size != sizeof(Response)) {
    return false;
  }
  if (response.type == MessageType::progress) {
    return response.error == 0 && response.native_error == 0 &&
           response.value == 0 && response.percent <= 100 &&
           valid_stage(response.stage);
  }
  // A failure measured nothing, so a number arriving with one is a protocol
  // error rather than something to read.
  if (response.type == MessageType::result && response.error != 0) {
    return response.value == 0;
  }
  return response.type == MessageType::result;
}

void write_exact(const HANDLE pipe, const void* const data,
                 const std::size_t size) {
  const auto* cursor = static_cast<const std::byte*>(data);
  std::size_t remaining = size;
  while (remaining != 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (::WriteFile(pipe, cursor, chunk, &written, nullptr) == FALSE) {
      throw_last_error("write volume-creation IPC");
    }
    if (written == 0) {
      throw_protocol_error("volume-creation IPC made no write progress");
    }
    cursor += written;
    remaining -= written;
  }
}

void read_exact(const HANDLE pipe, void* const data, const std::size_t size) {
  auto* cursor = static_cast<std::byte*>(data);
  std::size_t remaining = size;
  while (remaining != 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (::ReadFile(pipe, cursor, chunk, &read, nullptr) == FALSE) {
      throw_last_error("read volume-creation IPC");
    }
    if (read == 0) {
      throw_protocol_error("volume-creation IPC closed early");
    }
    cursor += read;
    remaining -= read;
  }
}

void write_request(const HANDLE pipe, const RequestHeader& header,
                   const std::wstring_view target_path,
                   const security::SecureBuffer& first_password,
                   const security::SecureBuffer& second_password,
                   std::wstring_view source_path, std::wstring_view destination_relative,
                   const PartitionTarget* partition_target) {
  if (!valid_request_header(header) ||
      target_path.size() != header.path_characters ||
      source_path.size() != header.source_characters || destination_relative.size() != header.destination_characters ||
      first_password.size() != header.first_password_bytes ||
      second_password.size() != header.second_password_bytes) {
    throw std::invalid_argument("invalid volume-creation request");
  }
  if ((partition_operation(header.operation)) != (partition_target != nullptr) ||
      (partition_target && (!valid_partition_target(*partition_target) || !valid_partition_path(target_path))))
    throw std::invalid_argument("invalid partition check request");
  write_exact(pipe, &header, sizeof(header));
  if (partition_target) write_exact(pipe, partition_target, sizeof(*partition_target));
  write_exact(pipe, target_path.data(),
              target_path.size() * sizeof(wchar_t));
  write_exact(pipe, first_password.data(), first_password.size());
  write_exact(pipe, second_password.data(), second_password.size());
  write_exact(pipe, source_path.data(), source_path.size() * sizeof(wchar_t));
  write_exact(pipe, destination_relative.data(), destination_relative.size() * sizeof(wchar_t));
}

Request read_request(
    const HANDLE pipe,
    const std::array<std::byte, kNonceSize>& expected_nonce) {
  RequestHeader header{};
  constexpr auto prefix_bytes = offsetof(RequestHeader, operation);
  read_exact(pipe, &header, prefix_bytes);
  if (header.magic != kProtocolMagic || header.version != kProtocolVersion || header.header_size != sizeof(header))
    throw_protocol_error("unsupported creation protocol prefix");
  read_exact(pipe, reinterpret_cast<std::byte*>(&header) + prefix_bytes, sizeof(header) - prefix_bytes);
  if (!valid_request_header(header) || header.nonce != expected_nonce) {
    throw_protocol_error("invalid volume-creation request header");
  }

  std::optional<PartitionTarget> target;
  if (header.partition_target_bytes) {
    target.emplace();
    read_exact(pipe, &*target, sizeof(*target));
    if (!valid_partition_target(*target)) throw_protocol_error("invalid partition target facts");
  }
  Request request{
      .operation = header.operation,
      .outer_size_bytes = header.outer_size_bytes,
      .hidden_size_bytes = header.hidden_size_bytes,
      .mounted_drive = header.mounted_drive,
      .mount_id = header.mount_id,
      .target_path = std::wstring(header.path_characters, L'\0'),
      .source_path = std::wstring(header.source_characters, L'\0'),
      .destination_relative = std::wstring(header.destination_characters, L'\0'),
      .first_password = security::SecureBuffer(header.first_password_bytes),
      .second_password =
          security::SecureBuffer(header.second_password_bytes),
      .nonce = header.nonce,
      .partition_target = target,
  };
  read_exact(pipe, request.target_path.data(),
             request.target_path.size() * sizeof(wchar_t));
  read_exact(pipe, request.first_password.data(), request.first_password.size());
  read_exact(pipe, request.second_password.data(),
             request.second_password.size());

  read_exact(pipe, request.source_path.data(), request.source_path.size() * sizeof(wchar_t));
  read_exact(pipe, request.destination_relative.data(), request.destination_relative.size() * sizeof(wchar_t));

  if (request.source_path.find(L'\0') != std::wstring::npos || request.destination_relative.find(L'\0') != std::wstring::npos || request.target_path.find(L'\0') != std::wstring::npos) {
    throw_protocol_error("target path contains an embedded NUL");
  }
  if ((partition_operation(request.operation)) && !valid_partition_path(request.target_path))
    throw_protocol_error("invalid partition volume name");
  return request;
}

void write_partition_confirmation(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, bool confirmed) {
  const PartitionConfirmation value{.nonce = nonce, .erase_all_data = confirmed ? 1u : 0u};
  write_exact(pipe, &value, sizeof(value));
}
bool read_partition_confirmation(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce) {
  // A partial/absent confirmation never extends this deadline or reaches writes.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    DWORD bytes{};
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr))
      throw_last_error("read partition confirmation");
    if (bytes >= sizeof(PartitionConfirmation)) break;
    if (std::chrono::steady_clock::now() >= deadline)
      throw_protocol_error("partition confirmation timed out");
    ::Sleep(10);
  }
  PartitionConfirmation value{};
  read_exact(pipe, &value, sizeof(value));
  if (value.magic != PartitionConfirmation{}.magic || value.version != kProtocolVersion ||
      value.size != sizeof(value) || value.nonce != nonce || value.erase_all_data > 1)
    throw_protocol_error("invalid partition confirmation");
  return value.erase_all_data == 1;
}

void write_response(const HANDLE pipe, const Response& response) {
  if (!valid_response(response)) {
    throw std::invalid_argument("invalid volume-creation response");
  }
  write_exact(pipe, &response, sizeof(response));
}

Response read_response(const HANDLE pipe) {
  Response response{};
  read_exact(pipe, &response, sizeof(response));
  if (!valid_response(response)) {
    throw_protocol_error("invalid volume-creation response");
  }
  return response;
}

bool valid_cancel_request(
    const CancelRequest& request,
    const std::array<std::byte, kNonceSize>& expected_nonce) noexcept {
  return request.magic == kProtocolMagic &&
         request.version == kProtocolVersion &&
         request.size == sizeof(CancelRequest) &&
         request.nonce == expected_nonce;
}

void write_cancel_request(const HANDLE pipe,
                          const std::array<std::byte, kNonceSize>& nonce) {
  CancelRequest request{};
  request.nonce = nonce;
  write_exact(pipe, &request, sizeof(request));
}

void write_pause_request(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce) {
  CancelRequest request{.magic = 0x32504355U, .nonce = nonce};
  write_exact(pipe, &request, sizeof(request));
}
unsigned inplace_control_requested(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce) {
  DWORD available{};
  if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) throw_last_error("in-place control pipe");
  if (available < sizeof(CancelRequest)) return 0;
  CancelRequest request{}; read_exact(pipe, &request, sizeof(request));
  const bool pause = request.magic == 0x32504355U;
  if (pause) request.magic = kProtocolMagic;
  if (!valid_cancel_request(request, nonce)) throw_protocol_error("invalid in-place control request");
  return pause ? 1u : 2u;
}

bool cancel_requested(
    const HANDLE pipe,
    const std::array<std::byte, kNonceSize>& expected_nonce) {
  DWORD available = 0;
  if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ==
      FALSE) {
    throw_last_error("peek volume-creation IPC");
  }
  // A partial message means the caller is still writing one. Reading it now
  // would block inside the format backend, so the next progress tick asks
  // again; nothing else is ever sent in this direction after the request.
  if (available < sizeof(CancelRequest)) {
    return false;
  }
  CancelRequest request{};
  read_exact(pipe, &request, sizeof(request));
  if (!valid_cancel_request(request, expected_nonce)) {
    throw_protocol_error("invalid volume-creation cancel request");
  }
  return true;
}

}  // namespace usb_vault::volume_creation::detail
