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
using usb_vault::card::vikey::kVikeyUserPinBytes;
using usb_vault::card::vikey::kVikeyUserSnBytes;
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
  request.version = static_cast<std::uint16_t>(kVersion + 1);
  expect(!valid_request(request), "unknown request version was accepted");
  request = Request{};
  request.operation = static_cast<Operation>(99);
  expect(!valid_request(request), "unknown request operation was accepted");
  request = Request{};
  request.payload_bytes = 1;
  expect(!valid_request(request), "probe request payload was accepted");
  request = Request{};
  request.operation = Operation::development_session_probe;
  expect(!valid_request(request), "missing session payload was accepted");
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
  response.security_properties =
      property_bit(SecurityProperty::filtered_token);
  expect(!valid_response(response, Operation::probe),
         "error response with security properties was accepted");
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
    test_development_session_request_rejects_wrong_sizes();
    test_response_round_trip();
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
