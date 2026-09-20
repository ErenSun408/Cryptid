#include "volume_creation/creator_protocol.h"
#include "volume_creation/creator_pipe_security.h"
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
#include <vector>

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
  expect(decoded.nonce == nonce, "the request nonce did not round-trip");
}

void test_cancel_request_round_trip() {
  const Pipe pipe = make_pipe();
  std::array<std::byte, kNonceSize> nonce{};
  nonce[3] = std::byte{0x7c};
  // Nothing has been sent, so nothing is pending. The helper asks this on
  // every progress tick from inside the format backend, so an empty pipe has
  // to answer immediately rather than wait for a cancel that may never come.
  expect(!cancel_requested(pipe.read.get(), nonce),
         "an empty pipe reported a pending cancel");
  write_cancel_request(pipe.write.get(), nonce);
  expect(cancel_requested(pipe.read.get(), nonce),
         "a written cancel was not seen");
  expect(!cancel_requested(pipe.read.get(), nonce),
         "the same cancel was reported twice");
}

void test_cancel_request_rejects_another_nonce() {
  const Pipe pipe = make_pipe();
  std::array<std::byte, kNonceSize> mine{};
  mine[0] = std::byte{0x11};
  std::array<std::byte, kNonceSize> other{};
  other[0] = std::byte{0x22};
  write_cancel_request(pipe.write.get(), other);
  bool rejected = false;
  try {
    static_cast<void>(cancel_requested(pipe.read.get(), mine));
  } catch (const std::system_error& error) {
    rejected = error.code() == make_error_code(CreationError::protocol_error);
  }
  expect(rejected, "a cancel carrying another nonce was accepted");
}

void test_partial_cancel_request_is_not_a_cancel() {
  const Pipe pipe = make_pipe();
  std::array<std::byte, kNonceSize> nonce{};
  nonce[5] = std::byte{0x5a};
  CancelRequest request{};
  request.nonce = nonce;
  const std::size_t half = sizeof(request) / 2;
  const auto* const bytes = reinterpret_cast<const std::byte*>(&request);
  write_exact(pipe.write.get(), bytes, half);
  // Reading half a message would mean blocking inside the format backend
  // until the rest arrived, which is exactly what must never happen here.
  expect(!cancel_requested(pipe.read.get(), nonce),
         "a half-written cancel was treated as one");
  write_exact(pipe.write.get(), bytes + half, sizeof(request) - half);
  expect(cancel_requested(pipe.read.get(), nonce),
         "the completed cancel was not seen");
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

void test_a_measurement_carries_no_secret_and_no_size() {
  RequestHeader scan{
      .operation = Operation::scan_tail_space,
      .path_characters = 3,
  };
  expect(valid_request_header(scan), "valid tail-space header rejected");

  RequestHeader sized = scan;
  sized.outer_size_bytes = 512;
  expect(!valid_request_header(sized), "a measurement accepted a size");

  RequestHeader keyed = scan;
  keyed.first_password_bytes = 8;
  expect(!valid_request_header(keyed), "a measurement accepted a password");

  RequestHeader pathless = scan;
  pathless.path_characters = 0;
  expect(!valid_request_header(pathless), "a measurement accepted no volume");
}

void test_a_measurement_round_trips_with_neither_password() {
  const Pipe pipe = make_pipe();
  const std::wstring root = L"Z:\\";
  std::array<std::byte, kNonceSize> nonce{};
  nonce[2] = std::byte{0x3b};
  const RequestHeader header{
      .operation = Operation::scan_tail_space,
      .path_characters = static_cast<std::uint32_t>(root.size()),
      .nonce = nonce,
  };
  write_request(pipe.write.get(), header, root, SecureBuffer{},
                SecureBuffer{});
  const Request received = read_request(pipe.read.get(), nonce);
  expect(received.operation == Operation::scan_tail_space,
         "the measurement operation did not round-trip");
  expect(received.target_path == root, "the measured volume did not arrive");
  expect(received.first_password.empty() && received.second_password.empty(),
         "a measurement arrived carrying a password");
}

void test_a_measured_number_travels_only_with_success() {
  const Pipe pipe = make_pipe();
  const Response sent{
      .type = MessageType::result,
      .value = 5ULL * 1024ULL * 1024ULL * 1024ULL,
  };
  write_response(pipe.write.get(), sent);
  const Response received = read_response(pipe.read.get());
  expect(received.value == sent.value, "the measured value did not arrive");

  Response failed = sent;
  failed.error = static_cast<std::int32_t>(CreationError::bitmap_scan_failed);
  expect(!valid_response(failed), "a failure arrived carrying a number");

  Response reporting{.type = MessageType::progress,
                     .stage = CreationStage::scanning_tail_space,
                     .percent = 10,
                     .value = 7};
  expect(!valid_response(reporting), "a progress report carried a number");
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

[[nodiscard]] std::vector<std::byte> current_token_user() {
  HANDLE raw_token = INVALID_HANDLE_VALUE;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) ==
      FALSE) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "OpenProcessToken");
  }
  const Handle token(raw_token);
  DWORD needed = 0;
  static_cast<void>(
      ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed));
  if (needed == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw std::runtime_error("measure current token user");
  }
  std::vector<std::byte> result(needed);
  if (::GetTokenInformation(token.get(), TokenUser, result.data(), needed,
                            &needed) == FALSE) {
    throw std::runtime_error("read current token user");
  }
  return result;
}

void test_pipe_dacl_names_system_and_token_user_explicitly() {
  auto descriptor =
      usb_vault::volume_creation::detail::
          current_user_pipe_security_descriptor();
  expect(descriptor.get() != nullptr,
         "current-user pipe security descriptor was unavailable");

  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision = 0;
  expect(::GetSecurityDescriptorControl(descriptor.get(), &control,
                                        &revision) != FALSE &&
             (control & SE_DACL_PROTECTED) != 0,
         "pipe DACL is not protected from inherited access");

  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  expect(::GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl,
                                     &defaulted) != FALSE &&
             present != FALSE && dacl != nullptr,
         "pipe security descriptor omitted its DACL");
  ACL_SIZE_INFORMATION information{};
  expect(::GetAclInformation(dacl, &information, sizeof(information),
                             AclSizeInformation) != FALSE &&
             information.AceCount == 2,
         "pipe DACL did not contain exactly two allow entries");

  const std::vector<std::byte> token_buffer = current_token_user();
  const auto* const token_user =
      reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
  std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid{};
  DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
  expect(::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(),
                              &system_sid_size) != FALSE,
         "cannot create LocalSystem SID for pipe DACL test");

  bool found_system = false;
  bool found_user = false;
  for (DWORD index = 0; index < information.AceCount; ++index) {
    void* raw_ace = nullptr;
    expect(::GetAce(dacl, index, &raw_ace) != FALSE && raw_ace != nullptr,
           "cannot inspect pipe DACL entry");
    const auto* const ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    expect(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
               (ace->Mask & GENERIC_ALL) != 0,
           "pipe DACL contains a non-full-access allow entry");
    const PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (::EqualSid(sid, system_sid.data()) != FALSE) {
      expect(!found_system, "pipe DACL repeats the LocalSystem SID");
      found_system = true;
    } else if (::EqualSid(sid, token_user->User.Sid) != FALSE) {
      expect(!found_user, "pipe DACL repeats the token user SID");
      found_user = true;
    } else {
      fail("pipe DACL grants an unexpected principal");
    }
  }
  expect(found_system && found_user,
         "pipe DACL did not explicitly grant system and token user");
}

}  // namespace

int main() {
  try {
    test_binary_password_round_trip();
    test_cancel_request_round_trip();
    test_cancel_request_rejects_another_nonce();
    test_partial_cancel_request_is_not_a_cancel();
    test_header_validation();
    test_nonce_mismatch_is_rejected();
    test_a_measurement_carries_no_secret_and_no_size();
    test_a_measurement_round_trips_with_neither_password();
    test_a_measured_number_travels_only_with_success();
    test_response_round_trip();
    test_format_error_mapping();
    test_pipe_dacl_names_system_and_token_user_explicitly();
    std::cout << "creator_protocol_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "creator_protocol_tests failed: " << error.what() << '\n';
    return 1;
  }
}
