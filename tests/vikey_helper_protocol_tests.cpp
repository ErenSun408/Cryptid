#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::CardError;
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
  request.version = 2;
  expect(!valid_request(request), "unknown request version was accepted");
  request = Request{};
  request.operation = static_cast<Operation>(99);
  expect(!valid_request(request), "unknown request operation was accepted");
  request = Request{};
  request.reserved = 1;
  expect(!valid_request(request), "nonzero request reserve was accepted");
}

void test_response_round_trip() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kRequiredSecurityProperties;
  sent.nonce[1] = std::byte{0x5a};
  write_response(pipe.write.get(), sent);
  const Response received = read_response(pipe.read.get(), sent.nonce);
  expect(received.security_properties == kRequiredSecurityProperties,
         "response security properties did not round-trip");
}

void test_response_validation() {
  Response response{};
  response.security_properties = kRequiredSecurityProperties;
  expect(valid_response(response), "valid response was rejected");

  response.security_properties = kKnownSecurityProperties | (1U << 31);
  expect(!valid_response(response), "unknown security property was accepted");
  response = Response{};
  response.status = static_cast<std::int32_t>(CardError::backend_unavailable);
  expect(valid_response(response), "known helper error was rejected");
  response.security_properties =
      property_bit(SecurityProperty::filtered_token);
  expect(!valid_response(response),
         "error response with security properties was accepted");
  response = Response{};
  response.status = 999;
  expect(!valid_response(response), "unknown helper error was accepted");
}

void test_nonce_mismatch_is_rejected() {
  const Pipe pipe = make_pipe();
  Response sent{};
  sent.security_properties = kRequiredSecurityProperties;
  sent.nonce[0] = std::byte{1};
  write_response(pipe.write.get(), sent);
  std::array<std::byte, kNonceSize> expected{};
  expect_protocol_error(
      [&] { static_cast<void>(read_response(pipe.read.get(), expected)); },
      "response nonce mismatch was accepted");
}

}  // namespace

int main() {
  try {
    test_request_round_trip();
    test_request_validation();
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
