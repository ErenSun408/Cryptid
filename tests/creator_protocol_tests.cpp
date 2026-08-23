#include "volume_creation/creator_protocol.h"
#include "volume_creation/veracrypt_format_abi.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

using usb_vault::security::SecureBuffer;
using namespace usb_vault::volume_creation;
using namespace usb_vault::volume_creation::detail;

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
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "CreatePipe");
  }
  return {.read = Handle(read), .write = Handle(write)};
}

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

[[nodiscard]] SecureBuffer password(
    const std::initializer_list<unsigned int> values) {
  SecureBuffer result(values.size());
  std::size_t index = 0;
  for (const unsigned int value : values) {
    result.bytes()[index++] = static_cast<std::byte>(value);
  }
  return result;
}

void test_binary_password_round_trip() {
  const Pipe pipe = make_pipe();
  const SecureBuffer first = password({0x41, 0x00, 0x42, 0xff});
  const SecureBuffer second = password({0x00, 0x10, 0x20});
  std::array<std::byte, kNonceSize> nonce{};
  nonce[0] = std::byte{0xa5};
  const std::wstring path = L"D:\\vault\\container.hc";
  const RequestHeader header{
      .operation = Operation::create_with_hidden,
      .outer_size_bytes = 100ULL * 1024ULL * 1024ULL,
      .hidden_size_bytes = 0,
      .path_characters = static_cast<std::uint32_t>(path.size()),
      .first_password_bytes = static_cast<std::uint32_t>(first.size()),
      .second_password_bytes = static_cast<std::uint32_t>(second.size()),
      .nonce = nonce,
  };
  write_request(pipe.write.get(), header, path, first, second);
  Request decoded = read_request(pipe.read.get(), nonce);
  expect(decoded.operation == Operation::create_with_hidden,
         "operation did not round-trip");
  expect(decoded.target_path == path, "path did not round-trip");
  expect(decoded.first_password.size() == first.size(),
         "first password length changed");
  expect(decoded.second_password.size() == second.size(),
         "second password length changed");
  expect(std::memcmp(decoded.first_password.data(), first.data(), first.size()) ==
             0,
         "binary first password changed");
  expect(std::memcmp(decoded.second_password.data(), second.data(),
                     second.size()) == 0,
         "binary second password changed");
}

void test_header_validation() {
  RequestHeader standard{
      .operation = Operation::create_standard,
      .outer_size_bytes = 20ULL * 1024ULL * 1024ULL,
      .path_characters = 10,
      .first_password_bytes = 64,
  };
  expect(valid_request_header(standard), "valid standard header rejected");
  standard.second_password_bytes = 1;
  expect(!valid_request_header(standard),
         "standard request accepted a second password");

  RequestHeader hidden{
      .operation = Operation::create_with_hidden,
      .outer_size_bytes = 100ULL * 1024ULL * 1024ULL,
      .path_characters = 10,
      .first_password_bytes = 64,
      .second_password_bytes = 64,
  };
  expect(valid_request_header(hidden), "valid hidden header rejected");
  hidden.version = 99;
  expect(!valid_request_header(hidden), "unknown protocol version accepted");
}

void test_nonce_mismatch_is_rejected() {
  const Pipe pipe = make_pipe();
  const SecureBuffer first = password({1});
  const std::wstring path = L"D:\\x.hc";
  std::array<std::byte, kNonceSize> sent{};
  std::array<std::byte, kNonceSize> expected{};
  sent[0] = std::byte{1};
  const RequestHeader header{
      .operation = Operation::create_standard,
      .outer_size_bytes = 20ULL * 1024ULL * 1024ULL,
      .path_characters = static_cast<std::uint32_t>(path.size()),
      .first_password_bytes = 1,
      .nonce = sent,
  };
  write_request(pipe.write.get(), header, path, first, SecureBuffer{});
  try {
    static_cast<void>(read_request(pipe.read.get(), expected));
    fail("nonce mismatch was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == make_error_code(CreationError::protocol_error),
           "nonce mismatch returned the wrong error");
  }
}

void test_response_round_trip() {
  const Pipe pipe = make_pipe();
  const Response sent{
      .type = MessageType::progress,
      .stage = CreationStage::formatting_hidden,
      .percent = 73,
  };
  write_response(pipe.write.get(), sent);
  const Response received = read_response(pipe.read.get());
  expect(received.type == MessageType::progress,
         "response type did not round-trip");
  expect(received.stage == CreationStage::formatting_hidden,
         "response stage did not round-trip");
  expect(received.percent == 73, "response percent did not round-trip");

  Response invalid = sent;
  invalid.stage = static_cast<CreationStage>(999);
  expect(!valid_response(invalid), "unknown progress stage was accepted");
}

void test_format_error_mapping() {
  namespace format = usb_vault::volume_creation::detail::format_abi;
  expect(format::map_error(format::kInvalidParameter) ==
             CreationError::invalid_request,
         "invalid SDK parameter was not mapped");
  expect(format::map_error(format::kPasswordPolicy) ==
             CreationError::invalid_request,
         "SDK password policy was not mapped");
  expect(format::map_error(format::kAccessDenied) ==
             CreationError::target_access_denied,
         "SDK access denied was not mapped");
  expect(format::map_error(format::kUserAbort) ==
             CreationError::operation_cancelled,
         "SDK cancellation was not mapped");
  expect(format::map_error(9999) == CreationError::format_rejected,
         "unknown SDK error did not fail closed");
}

}  // namespace

int main() {
  try {
    test_binary_password_round_trip();
    test_header_validation();
    test_nonce_mismatch_is_rejected();
    test_response_round_trip();
    test_format_error_mapping();
    std::cout << "creator_protocol_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "creator_protocol_tests failed: " << error.what() << '\n';
    return 1;
  }
}
