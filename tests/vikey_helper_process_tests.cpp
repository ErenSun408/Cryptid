#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
using usb_vault::card::CardError;
using usb_vault::card::Sha256Digest;
using usb_vault::card::vikey::probe_vikey_helper;

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

class Algorithm final {
 public:
  Algorithm() {
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
            &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
      fail("could not open the test SHA-256 provider");
    }
  }
  ~Algorithm() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(::BCryptCloseAlgorithmProvider(handle_, 0));
    }
  }
  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

[[nodiscard]] Sha256Digest file_digest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const std::streamoff size = input.tellg();
  expect(input.good() && size > 0 &&
             static_cast<unsigned long long>(size) <=
                 std::numeric_limits<ULONG>::max(),
         "could not size the helper test artifact");
  std::vector<UCHAR> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(size));
  expect(input.good(), "could not read the helper test artifact");

  Algorithm algorithm;
  Sha256Digest digest{};
  if (!BCRYPT_SUCCESS(::BCryptHash(
          algorithm.get(), nullptr, 0, bytes.data(),
          static_cast<ULONG>(bytes.size()),
          reinterpret_cast<PUCHAR>(digest.data()),
          static_cast<ULONG>(digest.size())))) {
    fail("could not hash the helper test artifact");
  }
  return digest;
}

[[nodiscard]] bool current_process_token_is_restricted() {
  HANDLE token = INVALID_HANDLE_VALUE;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) ==
      FALSE) {
    fail("could not inspect the process token");
  }
  const bool restricted = ::IsTokenRestricted(token) != FALSE;
  static_cast<void>(::CloseHandle(token));
  return restricted;
}

void test_restricted_helper_probe(const std::filesystem::path& helper) {
  const auto result = probe_vikey_helper(helper, file_digest(helper), 5s);
  if (result.security_properties !=
      usb_vault::card::vikey::helper::kRequiredSecurityProperties) {
    std::cerr << "helper security properties: actual=0x" << std::hex
              << result.security_properties << " expected=0x"
              << usb_vault::card::vikey::helper::kRequiredSecurityProperties
              << std::dec << '\n';
  }
  expect(result.security_properties ==
             usb_vault::card::vikey::helper::kRequiredSecurityProperties,
         "helper did not report the exact required security boundary");
}

void test_relative_path_fails_closed() {
  try {
    static_cast<void>(probe_vikey_helper(L"usb_vault_vikey_helper.exe",
                                         Sha256Digest{}));
    fail("relative helper path was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "relative helper path returned the wrong error");
  }
}

void test_stalled_helper_times_out(const std::filesystem::path& fixture) {
  const auto started = std::chrono::steady_clock::now();
  try {
    static_cast<void>(probe_vikey_helper(fixture, file_digest(fixture),
                                         150ms));
    fail("stalled helper was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::communication_failed),
           "stalled helper returned the wrong error");
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(elapsed >= 100ms, "helper timeout fired implausibly early");
  expect(elapsed < 2s, "helper timeout did not remain bounded");
}

void test_digest_mismatch_fails_before_launch(
    const std::filesystem::path& fixture) {
  Sha256Digest wrong = file_digest(fixture);
  wrong.front() ^= std::byte{1};
  try {
    static_cast<void>(probe_vikey_helper(fixture, wrong, 500ms));
    fail("helper digest mismatch was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "helper digest mismatch returned the wrong error");
  }
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 3) {
      fail("expected helper and stalled fixture paths");
    }
    if (current_process_token_is_restricted()) {
      std::cout << "vikey_helper_process_tests skipped: parent token is "
                   "already restricted\n";
      return 77;
    }
    const std::filesystem::path helper =
        std::filesystem::absolute(argv[1]).lexically_normal();
    const std::filesystem::path stalled =
        std::filesystem::absolute(argv[2]).lexically_normal();
    test_restricted_helper_probe(helper);
    test_relative_path_fails_closed();
    test_digest_mismatch_fails_before_launch(stalled);
    test_stalled_helper_times_out(stalled);
    std::cout << "vikey_helper_process_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_process_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
