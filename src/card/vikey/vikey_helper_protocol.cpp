#include "card/diagnostic_error.h"
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
  throw DiagnosticError(make_error_code(CardError::communication_failed), DiagnosticStage::ipc_protocol,
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
      request.operation == Operation::development_blob_round_trip_probe ||
      request.operation == Operation::development_blob_read ||
      request.operation == Operation::development_blob_write ||
      request.operation == Operation::card_journal_block_write ||
      is_record_read(request.operation) ||
      is_record_write(request.operation) ||
      request.operation == Operation::card_identity_probe ||
      request.operation == Operation::card_media_probe ||
      request.operation == Operation::card_media_bootstrap ||
      request.operation == Operation::development_media_reset;
  std::uint32_t required_payload = 0;
  if (request.operation == Operation::development_session_probe ||
      request.operation == Operation::development_blob_read ||
      request.operation == Operation::card_identity_probe ||
      request.operation == Operation::card_media_probe) {
    required_payload = kDevelopmentSessionPayloadBytes;
  } else if (request.operation ==
                 Operation::development_blob_round_trip_probe ||
             request.operation == Operation::development_blob_write) {
    required_payload = kDevelopmentBlobRoundTripPayloadBytes;
  } else if (request.operation == Operation::card_journal_block_write) {
    required_payload = kCardJournalBlockWritePayloadBytes;
  } else if (is_record_read(request.operation)) {
    required_payload = kCardRecordReadPayloadBytes;
  } else if (is_record_write(request.operation)) {
    required_payload =
        request.payload_bytes == kCardRecordThreeSlotWritePayloadBytes
            ? kCardRecordThreeSlotWritePayloadBytes
            : kCardRecordWritePayloadBytes;
  } else if (request.operation == Operation::card_media_bootstrap) {
    required_payload = kCardMediaBootstrapPayloadBytes;
  } else if (request.operation == Operation::development_media_reset) {
    required_payload = kDevelopmentMediaResetPayloadBytes;
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
  if (response.diagnostic > static_cast<std::uint32_t>(DiagnosticStage::unexpected) ||
      response.native_kind > static_cast<std::uint32_t>(NativeCodeKind::process_exit) ||
      (response.diagnostic == 0 && response.native_kind != 0) ||
      (response.diagnostic != 0 && response.native_kind == 0 && response.detail != 0)) return false;
  if (response.diagnostic == static_cast<std::uint32_t>(DiagnosticStage::process_security)) {
    if (response.status != static_cast<std::int32_t>(CardError::backend_unavailable) ||
        response.security_step < 90 || response.security_step > 108) return false;
    const bool api = response.security_step >= 107;
    if (response.native_kind != (api ? static_cast<std::uint32_t>(NativeCodeKind::win32) : 0u)) return false;
  } else if (response.security_step != 0) return false;
  if (response.status == 0) {
    if (response.diagnostic != 0 || response.native_kind != 0) return false;
    if (response.security_properties == 0) return false;
    if (is_record_read(expected_operation)) {
      if (!valid_record_payload(expected_operation, response.payload_bytes))
        return false;
    } else if (response.payload_bytes !=
               (expected_operation == Operation::development_blob_read
                    ? static_cast<std::uint32_t>(kVikeyBlobBytes)
                    : 0U)) {
      return false;
    }
    if (expected_operation == Operation::development_session_probe) {
      return response.result == 0 && response.logical_state == 0 &&
             valid_development_session_observation(
                 unpack_development_session_observation(response.detail));
    }
    if (expected_operation == Operation::card_identity_probe) {
      return response.detail != 0 && response.result == 0 &&
             response.logical_state == 0;
    }
    if (expected_operation == Operation::card_media_probe) {
      if (response.detail == 0 ||
          !valid_card_media_state(response.result)) {
        return false;
      }
      const bool product_evidence =
          response.result == static_cast<std::uint32_t>(
                                 CardMediaState::product_evidence);
      return product_evidence
                 ? valid_card_logical_state(response.logical_state)
                 : response.logical_state == 0;
    }
    return response.detail == 0 && response.result == 0 &&
           response.logical_state == 0;
  }
  const auto first = static_cast<std::int32_t>(CardError::backend_unavailable);
  const auto last = static_cast<std::int32_t>(CardError::operation_timed_out);
  return response.status >= first && response.status <= last &&
         response.security_properties == 0 && response.payload_bytes == 0 &&
         response.result == 0 && response.logical_state == 0;
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

DevelopmentBlobWriteRequest::DevelopmentBlobWriteRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes),
      blob(kVikeyBlobBytes) {}

CardRecordSessionRequest::CardRecordSessionRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes) {}

CardMediaBootstrapRequest::CardMediaBootstrapRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes) {}

CardRecordWriteRequest::CardRecordWriteRequest()
    : user_sn(kVikeyUserSnBytes),
      api_key(kVikeyApiKeyBytes),
      user_pin(kVikeyUserPinBytes) {}

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

void write_card_identity_probe_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::card_identity_probe ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey card identity request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

DevelopmentSessionRequest read_card_identity_probe_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::card_identity_probe) {
    throw_protocol("reject invalid ViKey card identity request");
  }
  DevelopmentSessionRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  return result;
}

void write_card_media_probe_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::card_media_probe ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey card media request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

DevelopmentSessionRequest read_card_media_probe_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::card_media_probe) {
    throw_protocol("reject invalid ViKey card media request");
  }
  DevelopmentSessionRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  return result;
}

void write_card_media_bootstrap_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const std::uint32_t expected_hardware_id,
    const CardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::card_media_bootstrap ||
      expected_hardware_id == 0 ||
      !valid_bootstrap_media_state(expected_media_state) ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey card media bootstrap request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, message_bytes(expected_hardware_id));
  write_exact(pipe, message_bytes(expected_media_state));
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

CardMediaBootstrapRequest read_card_media_bootstrap_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::card_media_bootstrap) {
    throw_protocol("reject invalid ViKey card media bootstrap request");
  }
  CardMediaBootstrapRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, mutable_message_bytes(result.expected_hardware_id));
  read_exact(pipe, mutable_message_bytes(result.expected_media_state));
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  if (result.expected_hardware_id == 0 ||
      !valid_bootstrap_media_state(result.expected_media_state)) {
    throw_protocol("reject invalid ViKey card media bootstrap binding");
  }
  return result;
}

void write_development_media_reset_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const std::uint32_t expected_hardware_id,
    const CardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::development_media_reset ||
      expected_hardware_id == 0 ||
      !valid_development_reset_media_state(expected_media_state) ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey development media reset request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, message_bytes(expected_hardware_id));
  write_exact(pipe, message_bytes(expected_media_state));
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

CardMediaBootstrapRequest read_development_media_reset_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::development_media_reset) {
    throw_protocol("reject invalid ViKey development media reset request");
  }
  CardMediaBootstrapRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, mutable_message_bytes(result.expected_hardware_id));
  read_exact(pipe, mutable_message_bytes(result.expected_media_state));
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  if (result.expected_hardware_id == 0 ||
      !valid_development_reset_media_state(result.expected_media_state)) {
    throw_protocol("reject invalid ViKey development media reset binding");
  }
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

void write_development_blob_read_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_read ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey development blob read request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

DevelopmentSessionRequest read_development_blob_read_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_read) {
    throw_protocol("reject invalid ViKey development blob read request");
  }
  DevelopmentSessionRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  return result;
}

void write_development_blob_write_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& blob) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_write ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes ||
      blob.size() != kVikeyBlobBytes) {
    throw_protocol("refuse invalid ViKey development blob write request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
  write_exact(pipe, blob.bytes());
}

DevelopmentBlobWriteRequest read_development_blob_write_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      request.operation != Operation::development_blob_write) {
    throw_protocol("reject invalid ViKey development blob write request");
  }
  DevelopmentBlobWriteRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  read_exact(pipe, result.blob.bytes());
  return result;
}

void write_card_record_read_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  if (!valid_request(request) ||
      !is_record_read(request.operation) ||
      expected_hardware_id == 0 ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes) {
    throw_protocol("refuse invalid ViKey card record read request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, message_bytes(expected_hardware_id));
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
}

CardRecordSessionRequest read_card_record_read_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      !is_record_read(request.operation)) {
    throw_protocol("reject invalid ViKey card record read request");
  }
  CardRecordSessionRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, mutable_message_bytes(result.expected_hardware_id));
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  if (result.expected_hardware_id == 0) {
    throw_protocol("reject zero ViKey card record identity");
  }
  return result;
}

void write_card_record_write_request(
    const HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    const std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& record) {
  if (!valid_request(request) ||
      !is_record_write(request.operation) ||
      expected_hardware_id == 0 ||
      user_sn.size() != kVikeyUserSnBytes ||
      api_key.size() != kVikeyApiKeyBytes ||
      user_pin.size() != kVikeyUserPinBytes ||
      !valid_record_payload(request.operation, record.size()) ||
      request.payload_bytes !=
          kCardRecordReadPayloadBytes + record.size()) {
    throw_protocol("refuse invalid ViKey card record write request");
  }
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, vendor_digest);
  write_exact(pipe, message_bytes(expected_hardware_id));
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, user_pin.bytes());
  write_exact(pipe, record.bytes());
}

CardRecordWriteRequest read_card_record_write_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) ||
      !is_record_write(request.operation)) {
    throw_protocol("reject invalid ViKey card record write request");
  }
  CardRecordWriteRequest result;
  read_exact(pipe, result.vendor_digest);
  read_exact(pipe, mutable_message_bytes(result.expected_hardware_id));
  read_exact(pipe, result.user_sn.bytes());
  read_exact(pipe, result.api_key.bytes());
  read_exact(pipe, result.user_pin.bytes());
  // valid_request admitted only the two record lengths.
  result.record = security::SecureBuffer(request.payload_bytes -
                                         kCardRecordReadPayloadBytes);
  read_exact(pipe, result.record.bytes());
  if (result.expected_hardware_id == 0) {
    throw_protocol("reject zero ViKey card record identity");
  }
  return result;
}

void write_card_journal_block_request(const HANDLE pipe, const Request& request,
    const Sha256Digest& digest, const std::uint32_t hardware_id,
    const security::SecureBuffer& user_sn, const security::SecureBuffer& api_key,
    const security::SecureBuffer& pin, const security::SecureBuffer& expected,
    const std::uint32_t offset, const security::SecureBuffer& replacement) {
  if (!valid_request(request) || request.operation != Operation::card_journal_block_write ||
      hardware_id == 0 || user_sn.size() != kVikeyUserSnBytes || api_key.size() != kVikeyApiKeyBytes ||
      pin.size() != kVikeyUserPinBytes || expected.size() != kCardRecordJournalV2StorageSize ||
      !valid_card_journal_block(offset, replacement.size())) {
    throw_protocol("refuse invalid conditional journal request");
  }
  const auto length = static_cast<std::uint32_t>(replacement.size());
  security::SecureBuffer padded(kCardRecordV2Size);
  std::ranges::copy(replacement.bytes(), padded.bytes().begin());
  write_exact(pipe, message_bytes(request));
  write_exact(pipe, digest);
  write_exact(pipe, message_bytes(hardware_id));
  write_exact(pipe, user_sn.bytes());
  write_exact(pipe, api_key.bytes());
  write_exact(pipe, pin.bytes());
  write_exact(pipe, expected.bytes());
  write_exact(pipe, message_bytes(offset));
  write_exact(pipe, message_bytes(length));
  write_exact(pipe, padded.bytes());
}

CardJournalBlockWriteRequest read_card_journal_block_request(
    const HANDLE pipe, const Request& request) {
  if (!valid_request(request) || request.operation != Operation::card_journal_block_write) {
    throw_protocol("reject invalid conditional journal request");
  }
  CardJournalBlockWriteRequest result;
  // Reuse only the fixed credential body reader, after validating the actual
  // operation and full payload length above. It does not consume a header.
  Request credential_header = request;
  credential_header.operation = Operation::card_journal_snapshot;
  credential_header.payload_bytes = kCardRecordReadPayloadBytes;
  result.session = read_card_record_read_request(pipe, credential_header);
  read_exact(pipe, result.expected_snapshot.bytes());
  read_exact(pipe, mutable_message_bytes(result.offset));
  read_exact(pipe, mutable_message_bytes(result.length));
  read_exact(pipe, result.replacement.bytes());
  if (!valid_card_journal_block(result.offset, result.length) ||
      !std::ranges::all_of(result.replacement.bytes().subspan(result.length),
          [](std::byte b) { return b == std::byte{}; })) {
    throw_protocol("reject invalid conditional journal block or padding");
  }
  return result;
}

void write_response(const HANDLE pipe, const Response& response,
                    const Operation expected_operation) {
  if (!valid_response(response, expected_operation) ||
      response.payload_bytes != 0) {
    throw_protocol("refuse invalid ViKey helper response");
  }
  write_exact(pipe, message_bytes(response));
}

void write_development_blob_response(
    const HANDLE pipe, const Response& response,
    const security::SecureBuffer& blob) {
  if (!valid_response(response, Operation::development_blob_read) ||
      response.status != 0 || blob.size() != kVikeyBlobBytes) {
    throw_protocol("refuse invalid ViKey development blob response");
  }
  write_exact(pipe, message_bytes(response));
  write_exact(pipe, blob.bytes());
}

void write_card_record_response(
    const HANDLE pipe, const Response& response,
    const security::SecureBuffer& record, const Operation operation) {
  if (!is_record_read(operation) || !valid_response(response, operation) ||
      response.status != 0 || record.size() != response.payload_bytes) {
    throw_protocol("refuse invalid ViKey card record response");
  }
  write_exact(pipe, message_bytes(response));
  write_exact(pipe, record.bytes());
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

security::SecureBuffer read_development_blob_response_payload(
    const HANDLE pipe, const Response& response) {
  if (!valid_response(response, Operation::development_blob_read) ||
      response.status != 0) {
    throw_protocol("reject invalid ViKey development blob response");
  }
  security::SecureBuffer blob(kVikeyBlobBytes);
  read_exact(pipe, blob.bytes());
  return blob;
}

security::SecureBuffer read_card_record_response_payload(
    const HANDLE pipe, const Response& response, const Operation operation) {
  if (!is_record_read(operation) || !valid_response(response, operation) ||
      response.status != 0) {
    throw_protocol("reject invalid ViKey card record response");
  }
  security::SecureBuffer record(response.payload_bytes);
  read_exact(pipe, record.bytes());
  return record;
}

}  // namespace usb_vault::card::vikey::helper
