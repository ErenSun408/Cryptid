#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::Sha256Digest;
using usb_vault::card::vikey::kVikeyApiKeyBytes;
using usb_vault::card::vikey::kVikeyBlobBytes;
using usb_vault::card::vikey::kVikeyUserPinBytes;
using usb_vault::card::vikey::kVikeyUserSnBytes;
using usb_vault::card::kCardRecordV1Size;
using namespace usb_vault::card::vikey::helper;

class Handle final {
 public:
  explicit Handle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
      : value_(value) {}
  ~Handle() noexcept {
    if (value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(value_));
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  [[nodiscard]] HANDLE get() const noexcept { return value_; }

 private:
  HANDLE value_;
};

struct Pipe final {
  Handle read;
  Handle write;
};

[[nodiscard]] Pipe make_pipe() {
  HANDLE read = INVALID_HANDLE_VALUE;
  HANDLE write = INVALID_HANDLE_VALUE;
  if (::CreatePipe(&read, &write, nullptr, 0) == FALSE) {
    throw std::runtime_error("CreatePipe failed");
  }
  return {.read = Handle(read), .write = Handle(write)};
}

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

void expect_protocol_error(const auto& operation, const char* const message) {
  try {
    operation();
    fail(message);
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::communication_failed),
           "invalid helper message returned the wrong error");
  }
}

void test_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.nonce[0] = std::byte{0xa5};
  write_request(pipe.write.get(), sent);
  const Request received = read_request(pipe.read.get());
  expect(received.nonce == sent.nonce, "request nonce did not round-trip");
  expect(received.operation == Operation::probe,
         "request operation did not round-trip");
}

void test_request_validation() {
  Request request{};
  expect(valid_request(request), "valid request was rejected");
  request.operation = Operation::vendor_runtime_probe;
  expect(valid_request(request), "vendor runtime probe was rejected");
  request.operation = Operation::hid_access_probe;
  expect(valid_request(request), "HID access probe was rejected");
  request.operation = Operation::development_session_probe;
  request.payload_bytes = kDevelopmentSessionPayloadBytes;
  expect(valid_request(request), "development session request was rejected");
  request.operation = Operation::development_blob_round_trip_probe;
  request.payload_bytes = kDevelopmentBlobRoundTripPayloadBytes;
  expect(valid_request(request),
         "development blob request was rejected");
  request.operation = Operation::development_blob_read;
  request.payload_bytes = kDevelopmentBlobReadPayloadBytes;
  expect(valid_request(request),
         "development blob read request was rejected");
  request.operation = Operation::development_blob_write;
  request.payload_bytes = kDevelopmentBlobWritePayloadBytes;
  expect(valid_request(request),
         "development blob write request was rejected");
  request.operation = Operation::card_record_read;
  request.payload_bytes = kCardRecordReadPayloadBytes;
  expect(valid_request(request), "card record read request was rejected");
  request.operation = Operation::card_record_write;
  request.payload_bytes = kCardRecordWritePayloadBytes;
  expect(valid_request(request), "card record write request was rejected");
  request.operation = Operation::card_identity_probe;
  request.payload_bytes = kCardIdentityProbePayloadBytes;
  expect(valid_request(request), "card identity request was rejected");
  request.operation = Operation::card_media_probe;
  request.payload_bytes = kCardMediaProbePayloadBytes;
  expect(valid_request(request), "card media request was rejected");
  request.operation = Operation::card_media_bootstrap;
  request.payload_bytes = kCardMediaBootstrapPayloadBytes;
  expect(valid_request(request), "card media bootstrap was rejected");
  request.payload_bytes = 0;
  expect(!valid_request(request),
         "card media bootstrap without bindings was accepted");
  request.operation = Operation::card_record_read;
  request.payload_bytes = kDevelopmentSessionPayloadBytes;
  expect(!valid_request(request),
         "v9 card record read accepted the v6 payload shape");
  request.operation = Operation::card_record_write;
  request.payload_bytes =
      kDevelopmentSessionPayloadBytes +
      static_cast<std::uint32_t>(kCardRecordV1Size);
  expect(!valid_request(request),
         "v9 card record write accepted the v6 payload shape");
  request.operation = Operation::card_identity_probe;
  request.payload_bytes = kCardIdentityProbePayloadBytes;
  request.version = static_cast<std::uint16_t>(kVersion + 1);
  expect(!valid_request(request), "unknown request version was accepted");
  request.version = static_cast<std::uint16_t>(kVersion - 1);
  expect(!valid_request(request), "downgraded request version was accepted");
  request = Request{};
  request.operation = static_cast<Operation>(99);
  expect(!valid_request(request), "unknown request operation was accepted");
  request = Request{};
  request.payload_bytes = 1;
  expect(!valid_request(request), "probe request payload was accepted");
  request = Request{};
  request.operation = Operation::development_session_probe;
  expect(!valid_request(request), "missing session payload was accepted");
  request = Request{};
  request.operation = Operation::development_blob_round_trip_probe;
  request.payload_bytes = kDevelopmentSessionPayloadBytes;
  expect(!valid_request(request),
         "development blob request accepted the session payload size");
}

[[nodiscard]] usb_vault::security::SecureBuffer fixed_secret(
    const std::size_t size, const std::byte value) {
  usb_vault::security::SecureBuffer result(size);
  for (std::byte& byte : result.bytes()) byte = value;
  return result;
}

void expect_fixed_secret(const usb_vault::security::SecureBuffer& secret,
                         const std::byte value,
                         const char* const message) {
  for (const std::byte byte : secret.bytes()) {
    if (byte != value) fail(message);
  }
}

void test_development_session_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.operation = Operation::development_session_probe;
  sent.payload_bytes = kDevelopmentSessionPayloadBytes;
  sent.nonce[2] = std::byte{0x6c};
  Sha256Digest digest{};
  digest.front() = std::byte{0xd1};
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});

  write_development_session_request(pipe.write.get(), sent, digest, user_sn,
                                    api_key, user_pin);
  const Request received = read_request(pipe.read.get());
  DevelopmentSessionRequest body =
      read_development_session_request(pipe.read.get(), received);
  expect(received.nonce == sent.nonce, "session nonce did not round-trip");
  expect(body.vendor_digest == digest,
         "session vendor digest did not round-trip");
  expect_fixed_secret(body.user_sn, std::byte{'A'},
                      "session UserSN did not round-trip");
  expect_fixed_secret(body.api_key, std::byte{'B'},
                      "session API key did not round-trip");
  expect_fixed_secret(body.user_pin, std::byte{'P'},
                      "session PIN did not round-trip");
  expect_fixed_secret(user_sn, std::byte{'A'},
                      "session write changed the caller UserSN");
  expect_fixed_secret(api_key, std::byte{'B'},
                      "session write changed the caller API key");
  expect_fixed_secret(user_pin, std::byte{'P'},
                      "session write changed the caller PIN");
}

void test_card_identity_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.operation = Operation::card_identity_probe;
  sent.payload_bytes = kCardIdentityProbePayloadBytes;
  Sha256Digest digest{};
  digest[3] = std::byte{0x71};
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});

  write_card_identity_probe_request(pipe.write.get(), sent, digest, user_sn,
                                    api_key, user_pin);
  const Request received = read_request(pipe.read.get());
  DevelopmentSessionRequest body =
      read_card_identity_probe_request(pipe.read.get(), received);
  expect(body.vendor_digest == digest,
         "card identity digest did not round-trip");
  expect_fixed_secret(body.user_sn, std::byte{'A'},
                      "card identity UserSN did not round-trip");
  expect_fixed_secret(body.api_key, std::byte{'B'},
                      "card identity API key did not round-trip");
  expect_fixed_secret(body.user_pin, std::byte{'P'},
                      "card identity PIN did not round-trip");
}

void test_card_media_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.operation = Operation::card_media_probe;
  sent.payload_bytes = kCardMediaProbePayloadBytes;
  Sha256Digest digest{};
  digest[4] = std::byte{0x72};
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});

  write_card_media_probe_request(pipe.write.get(), sent, digest, user_sn,
                                 api_key, user_pin);
  const Request received = read_request(pipe.read.get());
  DevelopmentSessionRequest body =
      read_card_media_probe_request(pipe.read.get(), received);
  expect(body.vendor_digest == digest,
         "card media digest did not round-trip");
  expect_fixed_secret(body.user_sn, std::byte{'A'},
                      "card media UserSN did not round-trip");
  expect_fixed_secret(body.api_key, std::byte{'B'},
                      "card media API key did not round-trip");
  expect_fixed_secret(body.user_pin, std::byte{'P'},
                      "card media PIN did not round-trip");
}

void test_card_media_bootstrap_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.operation = Operation::card_media_bootstrap;
  sent.payload_bytes = kCardMediaBootstrapPayloadBytes;
  Sha256Digest digest{};
  digest[5] = std::byte{0x73};
  constexpr std::uint32_t kExpectedHardwareId = 0x12345678U;
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'Q'});

  write_card_media_bootstrap_request(
      pipe.write.get(), sent, digest, kExpectedHardwareId,
      CardMediaState::blank, user_sn, api_key, user_pin);
  const Request received = read_request(pipe.read.get());
  CardMediaBootstrapRequest body =
      read_card_media_bootstrap_request(pipe.read.get(), received);
  expect(body.vendor_digest == digest,
         "card bootstrap digest did not round-trip");
  expect(body.expected_hardware_id == kExpectedHardwareId,
         "card bootstrap identity did not round-trip");
  expect(body.expected_media_state == CardMediaState::blank,
         "card bootstrap media state did not round-trip");
  expect_fixed_secret(body.user_pin, std::byte{'Q'},
                      "card bootstrap PIN did not round-trip");

  const Pipe invalid_pipe = make_pipe();
  expect_protocol_error(
      [&] {
        write_card_media_bootstrap_request(
            invalid_pipe.write.get(), sent, digest, 0,
            CardMediaState::blank, user_sn, api_key, user_pin);
      },
      "zero card bootstrap identity was accepted");
  expect_protocol_error(
      [&] {
        write_card_media_bootstrap_request(
            invalid_pipe.write.get(), sent, digest, kExpectedHardwareId,
            CardMediaState::foreign_data, user_sn, api_key, user_pin);
      },
      "ineligible card bootstrap state was accepted");
}

void test_development_session_request_rejects_wrong_sizes() {
  const Pipe pipe = make_pipe();
  Request request{};
  request.operation = Operation::development_session_probe;
  request.payload_bytes = kDevelopmentSessionPayloadBytes;
  const Sha256Digest digest{};
  const auto short_user_sn =
      fixed_secret(kVikeyUserSnBytes - 1, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});
  expect_protocol_error(
      [&] {
        write_development_session_request(pipe.write.get(), request, digest,
                                          short_user_sn, api_key, user_pin);
      },
      "short session UserSN was accepted");
}

void test_development_blob_request_round_trip() {
  const Pipe pipe = make_pipe();
  Request sent{};
  sent.operation = Operation::development_blob_round_trip_probe;
  sent.payload_bytes = kDevelopmentBlobRoundTripPayloadBytes;
  sent.nonce[4] = std::byte{0x7a};
  Sha256Digest digest{};
  digest.back() = std::byte{0xe1};
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});
  const auto test_blob = fixed_secret(kVikeyBlobBytes, std::byte{0xa5});

  write_development_blob_round_trip_request(
      pipe.write.get(), sent, digest, user_sn, api_key, user_pin, test_blob);
  const Request received = read_request(pipe.read.get());
  DevelopmentBlobRoundTripRequest body =
      read_development_blob_round_trip_request(pipe.read.get(), received);
  expect(received.nonce == sent.nonce,
         "development blob nonce did not round-trip");
  expect(body.vendor_digest == digest,
         "development blob vendor digest did not round-trip");
  expect_fixed_secret(body.user_sn, std::byte{'A'},
                      "development blob UserSN did not round-trip");
  expect_fixed_secret(body.api_key, std::byte{'B'},
                      "development blob API key did not round-trip");
  expect_fixed_secret(body.user_pin, std::byte{'P'},
                      "development blob PIN did not round-trip");
  expect_fixed_secret(body.test_blob, std::byte{0xa5},
                      "development test blob did not round-trip");
  expect_fixed_secret(test_blob, std::byte{0xa5},
                      "development blob write changed caller data");
}

void test_development_blob_request_rejects_wrong_sizes() {
  const Pipe pipe = make_pipe();
  Request request{};
  request.operation = Operation::development_blob_round_trip_probe;
  request.payload_bytes = kDevelopmentBlobRoundTripPayloadBytes;
  const Sha256Digest digest{};
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});
  const auto short_blob = fixed_secret(kVikeyBlobBytes - 1, std::byte{0xa5});
  expect_protocol_error(
      [&] {
        write_development_blob_round_trip_request(
            pipe.write.get(), request, digest, user_sn, api_key, user_pin,
            short_blob);
      },
      "short development test blob was accepted");
}

void test_development_blob_read_write_requests() {
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});
  const auto blob = fixed_secret(kVikeyBlobBytes, std::byte{0xd5});
  Sha256Digest digest{};
  digest[3] = std::byte{0x44};

  const Pipe read_pipe = make_pipe();
  Request read_request_header{};
  read_request_header.operation = Operation::development_blob_read;
  read_request_header.payload_bytes = kDevelopmentBlobReadPayloadBytes;
  write_development_blob_read_request(
      read_pipe.write.get(), read_request_header, digest, user_sn, api_key,
      user_pin);
  const Request received_read_header = read_request(read_pipe.read.get());
  DevelopmentSessionRequest read_body =
      read_development_blob_read_request(read_pipe.read.get(),
                                         received_read_header);
  expect(read_body.vendor_digest == digest,
         "blob read digest did not round-trip");
  expect_fixed_secret(read_body.user_sn, std::byte{'A'},
                      "blob read UserSN did not round-trip");
  expect_fixed_secret(read_body.api_key, std::byte{'B'},
                      "blob read API key did not round-trip");
  expect_fixed_secret(read_body.user_pin, std::byte{'P'},
                      "blob read PIN did not round-trip");

  const Pipe write_pipe = make_pipe();
  Request write_request_header{};
  write_request_header.operation = Operation::development_blob_write;
  write_request_header.payload_bytes = kDevelopmentBlobWritePayloadBytes;
  write_development_blob_write_request(
      write_pipe.write.get(), write_request_header, digest, user_sn, api_key,
      user_pin, blob);
  const Request received_write_header = read_request(write_pipe.read.get());
  DevelopmentBlobWriteRequest write_body =
      read_development_blob_write_request(write_pipe.read.get(),
                                          received_write_header);
  expect(write_body.vendor_digest == digest,
         "blob write digest did not round-trip");
  expect_fixed_secret(write_body.user_sn, std::byte{'A'},
                      "blob write UserSN did not round-trip");
  expect_fixed_secret(write_body.api_key, std::byte{'B'},
                      "blob write API key did not round-trip");
  expect_fixed_secret(write_body.user_pin, std::byte{'P'},
                      "blob write PIN did not round-trip");
  expect_fixed_secret(write_body.blob, std::byte{0xd5},
                      "blob write data did not round-trip");
  expect_fixed_secret(blob, std::byte{0xd5},
                      "blob write changed caller data");
}

void test_card_record_read_write_requests() {
  const auto user_sn = fixed_secret(kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(kVikeyUserPinBytes, std::byte{'P'});
  const auto record = fixed_secret(kCardRecordV1Size, std::byte{0x6d});
  Sha256Digest digest{};
  digest[5] = std::byte{0x91};
  constexpr std::uint32_t kExpectedHardwareId = 0x12345678U;

  const Pipe read_pipe = make_pipe();
  Request read_header{};
  read_header.operation = Operation::card_record_read;
  read_header.payload_bytes = kCardRecordReadPayloadBytes;
  write_card_record_read_request(read_pipe.write.get(), read_header, digest,
                                 kExpectedHardwareId, user_sn, api_key,
                                 user_pin);
  const Request received_read_header = read_request(read_pipe.read.get());
  CardRecordSessionRequest read_body = read_card_record_read_request(
      read_pipe.read.get(), received_read_header);
  expect(read_body.vendor_digest == digest,
         "card record read digest did not round-trip");
  expect(read_body.expected_hardware_id == kExpectedHardwareId,
         "card record read identity did not round-trip");
  expect_fixed_secret(read_body.user_sn, std::byte{'A'},
                      "card record read UserSN did not round-trip");

  const Pipe write_pipe = make_pipe();
  Request write_header{};
  write_header.operation = Operation::card_record_write;
  write_header.payload_bytes = kCardRecordWritePayloadBytes;
  write_card_record_write_request(write_pipe.write.get(), write_header,
                                  digest, kExpectedHardwareId, user_sn,
                                  api_key, user_pin, record);
  const Request received_write_header = read_request(write_pipe.read.get());
  CardRecordWriteRequest write_body = read_card_record_write_request(
      write_pipe.read.get(), received_write_header);
  expect(write_body.vendor_digest == digest,
         "card record write digest did not round-trip");
  expect(write_body.expected_hardware_id == kExpectedHardwareId,
         "card record write identity did not round-trip");
  expect_fixed_secret(write_body.record, std::byte{0x6d},
                      "card record write data did not round-trip");
  expect_fixed_secret(record, std::byte{0x6d},
                      "card record write changed caller data");

  const auto short_record =
      fixed_secret(kCardRecordV1Size - 1, std::byte{0x6d});
  expect_protocol_error(
      [&] {
        write_card_record_write_request(
            write_pipe.write.get(), write_header, digest,
            kExpectedHardwareId, user_sn, api_key, user_pin, short_record);
      },
      "short card record was accepted");
  expect_protocol_error(
      [&] {
        write_card_record_read_request(
            read_pipe.write.get(), read_header, digest, 0, user_sn, api_key,
            user_pin);
      },
      "zero expected hardware identity was accepted");
}

void test_response_round_trip() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kRequiredSecurityProperties;
  sent.nonce[1] = std::byte{0x5a};
  write_response(pipe.write.get(), sent, Operation::probe);
  const Response received =
      read_response(pipe.read.get(), sent.nonce, Operation::probe);
  expect(received.security_properties == kRequiredSecurityProperties,
         "response security properties did not round-trip");
}

void test_card_media_response_round_trip() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kDevelopmentCompatibilitySecurityProperties;
  sent.detail = 0x12345678U;
  sent.result = static_cast<std::uint32_t>(CardMediaState::product_evidence);
  sent.nonce[3] = std::byte{0x6a};
  write_response(pipe.write.get(), sent, Operation::card_media_probe);
  const Response received = read_response(
      pipe.read.get(), sent.nonce, Operation::card_media_probe);
  expect(received.detail == sent.detail,
         "card media hardware identity did not round-trip");
  expect(received.result == sent.result,
         "card media state did not round-trip");
}

void test_development_blob_response_round_trip() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kDevelopmentCompatibilitySecurityProperties;
  sent.payload_bytes = static_cast<std::uint32_t>(kVikeyBlobBytes);
  sent.nonce[7] = std::byte{0x71};
  const auto blob = fixed_secret(kVikeyBlobBytes, std::byte{0x3c});

  write_development_blob_response(pipe.write.get(), sent, blob);
  const Response received = read_response(
      pipe.read.get(), sent.nonce, Operation::development_blob_read);
  auto received_blob =
      read_development_blob_response_payload(pipe.read.get(), received);
  expect_fixed_secret(received_blob, std::byte{0x3c},
                      "blob response payload did not round-trip");
  expect_fixed_secret(blob, std::byte{0x3c},
                      "blob response changed caller data");
}

void test_card_record_response_round_trip() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kDevelopmentCompatibilitySecurityProperties;
  sent.payload_bytes = static_cast<std::uint32_t>(kCardRecordV1Size);
  sent.nonce[8] = std::byte{0x42};
  const auto record = fixed_secret(kCardRecordV1Size, std::byte{0x7e});

  write_card_record_response(pipe.write.get(), sent, record);
  const Response received = read_response(
      pipe.read.get(), sent.nonce, Operation::card_record_read);
  auto received_record =
      read_card_record_response_payload(pipe.read.get(), received);
  expect_fixed_secret(received_record, std::byte{0x7e},
                      "card record response did not round-trip");
  expect_fixed_secret(record, std::byte{0x7e},
                      "card record response changed caller data");
}

void test_response_validation() {
  Response response{};
  response.security_properties = kRequiredSecurityProperties;
  expect(valid_response(response, Operation::probe),
         "valid response was rejected");

  response.security_properties = kKnownSecurityProperties | (1U << 31);
  expect(!valid_response(response, Operation::probe),
         "unknown security property was accepted");
  response = Response{};
  response.security_properties = kRequiredSecurityProperties;
  response.detail = ERROR_ACCESS_DENIED;
  expect(!valid_response(response, Operation::probe),
         "successful probe response retained a detail");
  expect(valid_response(response, Operation::development_session_probe),
         "successful session attempt observation was rejected");
  expect(!valid_response(
             response, Operation::development_blob_round_trip_probe),
         "successful blob response retained a nonzero detail");
  response.detail = 0;
  expect(!valid_response(response, Operation::card_identity_probe),
         "zero card identity response was accepted");
  response.detail = 0x12345678U;
  expect(valid_response(response, Operation::card_identity_probe),
         "nonzero card identity response was rejected");
  response.result = static_cast<std::uint32_t>(CardMediaState::blank);
  expect(valid_response(response, Operation::card_media_probe),
         "valid card media response was rejected");
  expect(!valid_response(response, Operation::card_identity_probe),
         "card identity response retained a media result");
  response.result = 0;
  expect(!valid_response(response, Operation::card_media_probe),
         "card media response without a state was accepted");
  response.result =
      static_cast<std::uint32_t>(CardMediaState::product_evidence) + 1U;
  expect(!valid_response(response, Operation::card_media_probe),
         "card media response with an unknown state was accepted");
  response.result = static_cast<std::uint32_t>(CardMediaState::blank);
  response.detail = 0;
  expect(!valid_response(response, Operation::card_media_probe),
         "card media response without a hardware identity was accepted");
  response.result = 0;
  response.detail = 0;
  expect(valid_response(response, Operation::card_media_bootstrap),
         "valid card media bootstrap response was rejected");
  expect(!valid_response(response, Operation::development_blob_read),
         "blob read response without a payload was accepted");
  response.payload_bytes = static_cast<std::uint32_t>(kVikeyBlobBytes);
  expect(valid_response(response, Operation::development_blob_read),
         "valid blob read response was rejected");
  expect(!valid_response(response, Operation::card_record_read),
         "card record read accepted a 32-byte payload");
  response.payload_bytes = static_cast<std::uint32_t>(kCardRecordV1Size);
  expect(valid_response(response, Operation::card_record_read),
         "valid card record response was rejected");
  const Pipe omitted_payload_pipe = make_pipe();
  expect_protocol_error(
      [&] {
        write_response(omitted_payload_pipe.write.get(), response,
                       Operation::development_blob_read);
      },
      "blob read response omitted its declared payload");
  response.payload_bytes = 0;
  const DevelopmentSessionObservation observation{
      .attempts_before = 200,
      .maximum_before = 200,
      .attempts_after = 199,
      .maximum_after = 200,
  };
  expect(unpack_development_session_observation(
             pack_development_session_observation(observation))
                 .attempts_after == observation.attempts_after,
         "session attempt observation did not round-trip");
  expect(valid_development_session_observation(observation),
         "consistent session attempt observation was rejected");
  const DevelopmentSessionObservation inconsistent{
      .attempts_before = 201,
      .maximum_before = 200,
      .attempts_after = 200,
      .maximum_after = 200,
  };
  expect(!valid_development_session_observation(inconsistent),
         "attempt observation above its maximum was accepted");
  response.detail = pack_development_session_observation(inconsistent);
  expect(!valid_response(response, Operation::development_session_probe),
         "inconsistent successful session response was accepted");
  response = Response{};
  response.status = static_cast<std::int32_t>(CardError::backend_unavailable);
  response.detail = ERROR_ACCESS_DENIED;
  expect(valid_response(response, Operation::probe),
         "known helper error was rejected");
  response.result = static_cast<std::uint32_t>(CardMediaState::blank);
  expect(!valid_response(response, Operation::card_media_probe),
         "failed card media response retained a state");
  response.result = 0;
  response.payload_bytes = static_cast<std::uint32_t>(kVikeyBlobBytes);
  expect(!valid_response(response, Operation::development_blob_read),
         "failed blob read response retained a payload");
  response.payload_bytes = 0;
  response.security_properties =
      property_bit(SecurityProperty::filtered_token);
  expect(!valid_response(response, Operation::probe),
         "error response with security properties was accepted");
  response = Response{};
  response.version = static_cast<std::uint16_t>(kVersion - 1);
  response.security_properties = kRequiredSecurityProperties;
  expect(!valid_response(response, Operation::probe),
         "downgraded response version was accepted");
  response = Response{};
  response.status = 999;
  expect(!valid_response(response, Operation::probe),
         "unknown helper error was accepted");
}

void test_nonce_mismatch_is_rejected() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kRequiredSecurityProperties;
  sent.nonce[0] = std::byte{1};
  write_response(pipe.write.get(), sent, Operation::probe);
  std::array<std::byte, kNonceSize> expected{};
  expect_protocol_error(
      [&] {
        static_cast<void>(
            read_response(pipe.read.get(), expected, Operation::probe));
      },
      "response nonce mismatch was accepted");
}

}  // namespace

int main() {
  try {
    test_request_round_trip();
    test_request_validation();
    test_development_session_request_round_trip();
    test_card_identity_request_round_trip();
    test_card_media_request_round_trip();
    test_card_media_bootstrap_request_round_trip();
    test_development_session_request_rejects_wrong_sizes();
    test_development_blob_request_round_trip();
    test_development_blob_request_rejects_wrong_sizes();
    test_development_blob_read_write_requests();
    test_card_record_read_write_requests();
    test_response_round_trip();
    test_card_media_response_round_trip();
    test_development_blob_response_round_trip();
    test_card_record_response_round_trip();
    test_response_validation();
    test_nonce_mismatch_is_rejected();
    std::cout << "vikey_helper_protocol_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_protocol_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
