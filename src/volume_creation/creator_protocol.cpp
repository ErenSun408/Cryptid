#include "volume_creation/creator_protocol.h"

#include <algorithm>
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
  return operation == Operation::create_standard ||
         operation == Operation::create_with_hidden;
}

[[nodiscard]] bool valid_stage(const CreationStage stage) noexcept {
  return stage == CreationStage::formatting ||
         stage == CreationStage::formatting_outer ||
         stage == CreationStage::scanning_tail_space ||
         stage == CreationStage::formatting_hidden ||
         stage == CreationStage::authorized || stage == CreationStage::allocated;
}

}  // namespace

bool valid_request_header(const RequestHeader& header) noexcept {
  if (header.magic != kProtocolMagic ||
      header.version != kProtocolVersion ||
      header.header_size != sizeof(RequestHeader) || header.reserved != 0 ||
      !valid_operation(header.operation) || header.outer_size_bytes == 0 ||
      header.path_characters == 0 ||
      header.path_characters > kMaxPathCharacters ||
      header.first_password_bytes == 0 ||
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
           response.percent <= 100 && valid_stage(response.stage);
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
                   const security::SecureBuffer& second_password) {
  if (!valid_request_header(header) ||
      target_path.size() != header.path_characters ||
      first_password.size() != header.first_password_bytes ||
      second_password.size() != header.second_password_bytes) {
    throw std::invalid_argument("invalid volume-creation request");
  }
  write_exact(pipe, &header, sizeof(header));
  write_exact(pipe, target_path.data(),
              target_path.size() * sizeof(wchar_t));
  write_exact(pipe, first_password.data(), first_password.size());
  write_exact(pipe, second_password.data(), second_password.size());
}

Request read_request(
    const HANDLE pipe,
    const std::array<std::byte, kNonceSize>& expected_nonce) {
  RequestHeader header{};
  read_exact(pipe, &header, sizeof(header));
  if (!valid_request_header(header) || header.nonce != expected_nonce) {
    throw_protocol_error("invalid volume-creation request header");
  }

  Request request{
      .operation = header.operation,
      .outer_size_bytes = header.outer_size_bytes,
      .hidden_size_bytes = header.hidden_size_bytes,
      .target_path = std::wstring(header.path_characters, L'\0'),
      .first_password = security::SecureBuffer(header.first_password_bytes),
      .second_password =
          security::SecureBuffer(header.second_password_bytes),
  };
  read_exact(pipe, request.target_path.data(),
             request.target_path.size() * sizeof(wchar_t));
  read_exact(pipe, request.first_password.data(), request.first_password.size());
  read_exact(pipe, request.second_password.data(),
             request.second_password.size());

  if (request.target_path.find(L'\0') != std::wstring::npos) {
    throw_protocol_error("target path contains an embedded NUL");
  }
  return request;
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

}  // namespace usb_vault::volume_creation::detail
