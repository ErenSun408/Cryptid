#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {

using namespace std::chrono_literals;
using usb_vault::card::CardError;
using usb_vault::card::vikey::probe_vikey_helper;

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
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
  const auto result = probe_vikey_helper(helper, 5s);
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
    static_cast<void>(probe_vikey_helper(L"usb_vault_vikey_helper.exe"));
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
    static_cast<void>(probe_vikey_helper(fixture, 150ms));
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
    test_stalled_helper_times_out(stalled);
    std::cout << "vikey_helper_process_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_process_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
