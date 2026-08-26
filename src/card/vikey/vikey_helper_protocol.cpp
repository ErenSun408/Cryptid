#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <system_error>

namespace usb_vault::card::vikey::helper {
namespace {

[[noreturn]] void throw_protocol(const char* const context) {
  throw std::system_error(make_error_code(CardError::communication_failed),
                          context);
}

void write_exact(const HANDLE pipe, const std::span<const std::byte> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const std::size_t remaining = bytes.size() - written;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD transferred = 0;
    if (::WriteFile(pipe, bytes.data() + written, chunk, &transferred,
                    nullptr) == FALSE ||
        transferred == 0 || transferred > chunk) {
      throw_protocol("write ViKey helper IPC message");
    }
    written += transferred;
  }
}

void read_exact(const HANDLE pipe, const std::span<std::byte> bytes) {
  std::size_t read = 0;
  while (read < bytes.size()) {
    const std::size_t remaining = bytes.size() - read;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD transferred = 0;
    if (::ReadFile(pipe, bytes.data() + read, chunk, &transferred, nullptr) ==
            FALSE ||
        transferred == 0 || transferred > chunk) {
      throw_protocol("read ViKey helper IPC message");
    }
    read += transferred;
  }
}

template <typename Message>
[[nodiscard]] std::span<const std::byte> message_bytes(
    const Message& message) noexcept {
  return {reinterpret_cast<const std::byte*>(&message), sizeof(message)};
}

template <typename Message>
[[nodiscard]] std::span<std::byte> mutable_message_bytes(
    Message& message) noexcept {
  return {reinterpret_cast<std::byte*>(&message), sizeof(message)};
}

}  // namespace

bool valid_request(const Request& request) noexcept {
  const bool known_operation =
      request.operation == Operation::probe ||
      request.operation == Operation::vendor_runtime_probe ||
      request.operation == Operation::hid_access_probe ||
      request.operation == Operation::development_session_probe ||
      request.operation == Operation::development_blob_round_trip_probe;
  std::uint32_t required_payload = 0;
  if (request.operation == Operation::development_session_probe) {
    required_payload = kDevelopmentSessionPayloadBytes;
  } else if (request.operation ==
             Operation::development_blob_round_trip_probe) {
    required_payload = kDevelopmentBlobRoundTripPayloadBytes;
  }
  return request.magic == kMagic && request.version == kVersion &&
         request.header_bytes == sizeof(Request) &&
         known_operation && request.payload_bytes == required_payload;
}

bool valid_response(const Response& response,
                    const Operation expected_operation) noexcept {
  if (response.magic != kMagic || response.version != kVersion ||
      response.header_bytes != sizeof(Response) ||
      (response.security_properties & ~kKnownSecurityProperties) != 0) {
    return false;
  }
  if (response.status == 0) {
    if (response.security_properties == 0) return false;
    if (expected_operation == Operation::development_session_probe) {
      return valid_development_session_observation(
          unpack_development_session_observation(response.detail));
    }
    return response.detail == 0;
  }
  const auto first = static_cast<std::int32_t>(CardError::backend_unavailable);
  const auto last = static_cast<std::int32_t>(CardError::device_failure);
  return response.status >= first && response.status <= last &&
         response.security_properties == 0;
}

void write_request(const HANDLE pipe, const Request& request) {
  if (!valid_request(request) || request.payload_bytes != 0) {
    throw_protocol("refuse invalid ViKey helper request");
  }
  write_exact(pipe, message_bytes(request));
}

Request read_request(const HANDLE pipe) {
  Request request{};
  read_exact(pipe, mutable_message_bytes(request));
  if (!valid_request(request)) {
    throw_protocol("reject invalid ViKey helper request");
  }
  return request;
}

DevelopmentSessionRequest::DevelopmentSessionRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes) {}

DevelopmentBlobRoundTripRequest::DevelopmentBlobRoundTripRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes),
      test_blob(kVikeyBlobBytes) {}

void write_development_session_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::development_session_probe ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey development session request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

DevelopmentSessionRequest read_development_session_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::development_session_probe) {
    throw_protocol("reject invalid ViKey development session request");
  }
  DevelopmentSessionRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  return result;
}

void write_development_blob_round_trip_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& test_blob) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_round_trip_probe ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes ||
      test_blob.size() != kVikeyBlobBytes) {
    throw_protocol("refuse invalid ViKey development blob request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
  write_exact(pipe, test_blob.bytes());
}

DevelopmentBlobRoundTripRequest read_development_blob_round_trip_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_round_trip_probe) {
    throw_protocol("reject invalid ViKey development blob request");
  }
  DevelopmentBlobRoundTripRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  read_exact(pipe, result.test_blob.bytes());
  return result;
}

void write_response(const HANDLE pipe, const Response& response,
                    const Operation expected_operation) {
  if (!valid_response(response, expected_operation)) {
    throw_protocol("refuse invalid ViKey helper response");
  }
  write_exact(pipe, message_bytes(response));
}

Response read_response(
    const HANDLE pipe,
    const std::array<std::byte, kNonceSize>& expected_nonce,
    const Operation expected_operation) {
  Response response{};
  read_exact(pipe, mutable_message_bytes(response));
  if (!valid_response(response, expected_operation) ||
      response.nonce != expected_nonce) {
    throw_protocol("reject invalid ViKey helper response");
  }
  return response;
}

}  // namespace usb_vault::card::vikey::helper
