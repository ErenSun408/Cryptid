#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_protocol.h"
#include "vikey_network_probe.h"

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
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace std::chrono_literals;
using usb_vault::card::CardError;
using usb_vault::card::Sha256Digest;
using usb_vault::card::vikey::
    probe_vikey_development_helper_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_session_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_vendor_runtime_with_network_access;
using usb_vault::card::vikey::probe_vikey_helper;
using usb_vault::card::vikey::probe_vikey_vendor_runtime;
using usb_vault::security::SecureBuffer;

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

void run_test(const char* const name, const auto& test) {
  try {
    test();
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(name) + ": " + error.what());
  }
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

class TemporaryNonExecutable final {
 public:
  explicit TemporaryNonExecutable(const std::filesystem::path& directory) {
    wchar_t path[MAX_PATH]{};
    if (::GetTempFileNameW(directory.c_str(), L"uvt", 0, path) == 0) {
      fail("could not create a temporary non-executable artifact");
    }
    path_ = path;
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    constexpr char kContents[] = "not a Windows executable";
    output.write(kContents, sizeof(kContents) - 1);
    if (!output.good()) {
      output.close();
      static_cast<void>(::DeleteFileW(path_.c_str()));
      fail("could not populate the temporary non-executable artifact");
    }
  }

  ~TemporaryNonExecutable() noexcept {
    if (!path_.empty()) static_cast<void>(::DeleteFileW(path_.c_str()));
  }

  TemporaryNonExecutable(const TemporaryNonExecutable&) = delete;
  TemporaryNonExecutable& operator=(const TemporaryNonExecutable&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
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

struct TokenPrivilege final {
  DWORD low_part{};
  LONG high_part{};
  DWORD attributes{};

  [[nodiscard]] bool operator==(const TokenPrivilege&) const = default;
};

[[nodiscard]] std::vector<TokenPrivilege> current_process_privileges() {
  HANDLE token = INVALID_HANDLE_VALUE;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) ==
      FALSE) {
    fail("could not open the parent token for privilege snapshot");
  }
  DWORD bytes = 0;
  static_cast<void>(
      ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &bytes));
  if (bytes < sizeof(TOKEN_PRIVILEGES) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    static_cast<void>(::CloseHandle(token));
    fail("could not size the parent privilege snapshot");
  }
  std::vector<std::byte> storage(bytes);
  if (::GetTokenInformation(token, TokenPrivileges, storage.data(), bytes,
                            &bytes) == FALSE) {
    static_cast<void>(::CloseHandle(token));
    fail("could not read the parent privilege snapshot");
  }
  static_cast<void>(::CloseHandle(token));

  const auto* const privileges =
      reinterpret_cast<const TOKEN_PRIVILEGES*>(storage.data());
  std::vector<TokenPrivilege> snapshot;
  snapshot.reserve(privileges->PrivilegeCount);
  for (DWORD index = 0; index < privileges->PrivilegeCount; ++index) {
    const LUID_AND_ATTRIBUTES& privilege = privileges->Privileges[index];
    snapshot.push_back({.low_part = privilege.Luid.LowPart,
                        .high_part = privilege.Luid.HighPart,
                        .attributes = privilege.Attributes});
  }
  return snapshot;
}

[[nodiscard]] std::size_t helper_profile_count() {
  const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  expect(required > 1, "could not resolve LOCALAPPDATA for profile test");
  std::wstring local_app_data(required, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data.data(), required);
  expect(written > 0 && written < required,
         "could not read LOCALAPPDATA for profile test");
  local_app_data.resize(written);

  const std::filesystem::path packages =
      std::filesystem::path(local_app_data) / L"Packages";
  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator entry(packages, error), end;
       !error && entry != end; entry.increment(error)) {
    if (entry->path().filename().native().starts_with(
            L"usb-vault-vikey-helper-")) {
      ++count;
    }
  }
  expect(!error, "could not enumerate AppContainer profiles for cleanup test");
  return count;
}

[[nodiscard]] std::size_t development_helper_directory_count() {
  const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  expect(required > 1,
         "could not resolve LOCALAPPDATA for development cleanup test");
  std::wstring local_app_data(required, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(
      L"LOCALAPPDATA", local_app_data.data(), required);
  expect(written > 0 && written < required,
         "could not read LOCALAPPDATA for development cleanup test");
  local_app_data.resize(written);

  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator entry(local_app_data, error), end;
       !error && entry != end; entry.increment(error)) {
    if (entry->path().filename().native().starts_with(
            L"usb-vault-vikey-development-helper-")) {
      ++count;
    }
  }
  expect(!error,
         "could not enumerate development helper directories for cleanup test");
  return count;
}

void test_restricted_helper_probe(const std::filesystem::path& helper) {
  const std::size_t profiles_before = helper_profile_count();
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
  expect(helper_profile_count() == profiles_before,
         "ephemeral helper AppContainer profile was left behind");
}

void test_network_isolation(const std::filesystem::path& fixture) {
  expect(usb_vault::card::vikey::test::ordinary_network_path_available(),
         "unisolated network positive control could not create an ordinary "
         "Winsock path");
  const std::size_t profiles_before = helper_profile_count();
  const auto result = probe_vikey_helper(fixture, file_digest(fixture), 5s);
  expect(result.security_properties ==
             usb_vault::card::vikey::helper::kRequiredSecurityProperties,
         "network fixture did not retain the complete helper boundary");
  expect(helper_profile_count() == profiles_before,
         "network fixture left an AppContainer profile behind");
}

void test_development_network_access(
    const std::filesystem::path& helper,
    const std::filesystem::path& network_enabled_fixture) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto parent_privileges_before = current_process_privileges();
  const auto helper_result =
      probe_vikey_development_helper_with_network_access(
          helper, file_digest(helper), 5s);
  // Development omits only the three AppContainer-specific assertions.
  if (helper_result.security_properties !=
      usb_vault::card::vikey::helper::
          kDevelopmentCompatibilitySecurityProperties) {
    std::cerr << "development helper security properties: actual=0x"
              << std::hex << helper_result.security_properties
              << " expected=0x"
              << usb_vault::card::vikey::helper::
                     kDevelopmentCompatibilitySecurityProperties
              << std::dec << '\n';
  }
  expect(helper_result.security_properties ==
             usb_vault::card::vikey::helper::
                 kDevelopmentCompatibilitySecurityProperties,
         "development helper did not retain its exact restricted boundary");
  expect(development_helper_directory_count() == directories_before,
         "development helper probe left a staging directory behind");
  expect(current_process_privileges() == parent_privileges_before,
         "development child privilege filtering modified the parent token");

  const auto network_result =
      probe_vikey_development_helper_with_network_access(
          network_enabled_fixture, file_digest(network_enabled_fixture), 5s);
  expect(network_result.security_properties ==
             usb_vault::card::vikey::helper::
                 kDevelopmentCompatibilitySecurityProperties,
         "network-enabled fixture did not retain the development boundary");
  expect(development_helper_directory_count() == directories_before,
         "network-enabled fixture left a staging directory behind");
}

void test_vendor_runtime_staging(const std::filesystem::path& helper,
                                 const std::filesystem::path& vendor) {
  const std::size_t profiles_before = helper_profile_count();
  const auto result = probe_vikey_vendor_runtime(
      helper, file_digest(helper), vendor.parent_path(), file_digest(vendor),
      5s);
  expect(result.security_properties ==
             usb_vault::card::vikey::helper::kRequiredSecurityProperties,
         "vendor runtime probe lost a required security property");
  expect(helper_profile_count() == profiles_before,
         "vendor runtime probe left an AppContainer profile behind");
}

void test_development_vendor_runtime_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto result =
      probe_vikey_development_vendor_runtime_with_network_access(
          helper, file_digest(helper), vendor.parent_path(),
          file_digest(vendor), 5s);
  expect(result.security_properties ==
             usb_vault::card::vikey::helper::
                 kDevelopmentCompatibilitySecurityProperties,
         "development vendor probe lost a restricted-process property");
  expect(development_helper_directory_count() == directories_before,
         "development vendor probe left a staging directory behind");
}

[[nodiscard]] SecureBuffer fixed_secret(const std::size_t size,
                                        const std::byte value) {
  SecureBuffer result(size);
  for (std::byte& byte : result.bytes()) byte = value;
  return result;
}

void expect_fixed_secret(const SecureBuffer& secret, const std::byte value,
                         const char* const message) {
  for (const std::byte byte : secret.bytes()) {
    if (byte != value) fail(message);
  }
}

void test_development_session_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(
      usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'P'});
  const auto result = probe_vikey_development_session_with_network_access(
      helper, file_digest(helper), vendor.parent_path(), file_digest(vendor),
      user_sn, api_key, user_pin, 5s);
  expect(result.security_properties ==
             usb_vault::card::vikey::helper::
                 kDevelopmentCompatibilitySecurityProperties,
         "development session lost a restricted-process property");
  expect(result.attempts_before == 200 && result.maximum_before == 200 &&
             result.attempts_after == 200 && result.maximum_after == 200,
         "development session returned the wrong PIN attempt observation");
  expect_fixed_secret(user_sn, std::byte{'A'},
                      "development session changed the caller UserSN");
  expect_fixed_secret(api_key, std::byte{'B'},
                      "development session changed the caller API key");
  expect_fixed_secret(user_pin, std::byte{'P'},
                      "development session changed the caller PIN");
  expect(development_helper_directory_count() == directories_before,
         "development session left a staging directory behind");
}

void test_development_session_rejects_invalid_credentials(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'});
  auto api_key = fixed_secret(
      usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'P'});
  api_key.bytes()[3] = std::byte{'G'};
  try {
    static_cast<void>(probe_vikey_development_session_with_network_access(
        helper, file_digest(helper), vendor.parent_path(), file_digest(vendor),
        user_sn, api_key, user_pin, 5s));
    fail("invalid development API key was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "invalid development API key returned the wrong generic error");
  }
  expect(development_helper_directory_count() == directories_before,
         "invalid development session left a staging directory behind");

  const auto short_user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes - 1, std::byte{'A'});
  api_key.bytes()[3] = std::byte{'B'};
  try {
    static_cast<void>(probe_vikey_development_session_with_network_access(
        helper, file_digest(helper), vendor.parent_path(), file_digest(vendor),
        short_user_sn, api_key, user_pin, 5s));
    fail("short development UserSN was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "short development UserSN returned the wrong generic error");
  }
  expect(development_helper_directory_count() == directories_before,
         "short development session created a staging directory");
}

void test_development_vendor_digest_mismatch_fails_before_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  Sha256Digest wrong = file_digest(vendor);
  wrong.front() ^= std::byte{1};
  const std::size_t directories_before =
      development_helper_directory_count();
  try {
    static_cast<void>(
        probe_vikey_development_vendor_runtime_with_network_access(
            helper, file_digest(helper), vendor.parent_path(), wrong, 500ms));
    fail("development vendor digest mismatch was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "development vendor digest mismatch returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "development vendor digest mismatch created a staging directory");
}

void test_development_helper_digest_mismatch_fails_before_staging(
    const std::filesystem::path& helper) {
  Sha256Digest wrong = file_digest(helper);
  wrong.front() ^= std::byte{1};
  const std::size_t directories_before =
      development_helper_directory_count();
  try {
    static_cast<void>(probe_vikey_development_helper_with_network_access(
        helper, wrong, 500ms));
    fail("development helper digest mismatch was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "development helper digest mismatch returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "development helper digest mismatch created a staging directory");
}

void test_development_stalled_helper_times_out(
    const std::filesystem::path& fixture) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto started = std::chrono::steady_clock::now();
  try {
    static_cast<void>(probe_vikey_development_helper_with_network_access(
        fixture, file_digest(fixture), 150ms));
    fail("stalled development helper was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::communication_failed),
           "stalled development helper returned the wrong error");
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(elapsed >= 100ms,
         "development helper timeout fired implausibly early");
  expect(elapsed < 2s,
         "development helper timeout did not remain bounded");
  expect(development_helper_directory_count() == directories_before,
         "timed-out development helper left a staging directory behind");
}

void test_development_launch_failure_removes_directory(
    const std::filesystem::path& artifact_directory) {
  const TemporaryNonExecutable artifact(artifact_directory);
  const std::size_t directories_before =
      development_helper_directory_count();
  try {
    static_cast<void>(probe_vikey_development_helper_with_network_access(
        artifact.path(), file_digest(artifact.path()), 1s));
    fail("non-executable development helper artifact was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "non-executable development helper returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "failed development helper launch left a staging directory behind");
}

void test_vendor_digest_mismatch_fails_before_launch(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  Sha256Digest wrong = file_digest(vendor);
  wrong.front() ^= std::byte{1};
  const std::size_t profiles_before = helper_profile_count();
  try {
    static_cast<void>(probe_vikey_vendor_runtime(
        helper, file_digest(helper), vendor.parent_path(), wrong, 500ms));
    fail("vendor digest mismatch was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "vendor digest mismatch returned the wrong error");
  }
  expect(helper_profile_count() == profiles_before,
         "vendor digest mismatch created an AppContainer profile");
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
  const std::size_t profiles_before = helper_profile_count();
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
  expect(helper_profile_count() == profiles_before,
         "timed-out helper left an AppContainer profile behind");
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

void test_launch_failure_removes_profile(
    const std::filesystem::path& artifact_directory) {
  const TemporaryNonExecutable artifact(artifact_directory);
  const std::size_t profiles_before = helper_profile_count();
  try {
    static_cast<void>(
        probe_vikey_helper(artifact.path(), file_digest(artifact.path()), 1s));
    fail("non-executable helper artifact was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "non-executable helper returned the wrong error");
  }
  expect(helper_profile_count() == profiles_before,
         "failed helper launch left an AppContainer profile behind");
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 6) {
      fail("expected helper, isolated-network, network-enabled, stalled, "
           "and vendor fixture paths");
    }
    if (current_process_token_is_restricted()) {
      std::cout << "vikey_helper_process_tests skipped: parent token is "
                   "already restricted\n";
      return 77;
    }
    const std::filesystem::path helper =
        std::filesystem::absolute(argv[1]).lexically_normal();
    const std::filesystem::path network =
        std::filesystem::absolute(argv[2]).lexically_normal();
    const std::filesystem::path network_enabled =
        std::filesystem::absolute(argv[3]).lexically_normal();
    const std::filesystem::path stalled =
        std::filesystem::absolute(argv[4]).lexically_normal();
    const std::filesystem::path vendor =
        std::filesystem::absolute(argv[5]).lexically_normal();
    run_test("restricted helper probe",
             [&] { test_restricted_helper_probe(helper); });
    run_test("vendor runtime staging",
             [&] { test_vendor_runtime_staging(helper, vendor); });
    run_test("vendor digest mismatch", [&] {
      test_vendor_digest_mismatch_fails_before_launch(helper, vendor);
    });
    run_test("network isolation", [&] { test_network_isolation(network); });
    run_test("development network access",
             [&] { test_development_network_access(helper, network_enabled); });
    run_test("development vendor runtime staging", [&] {
      test_development_vendor_runtime_staging(helper, vendor);
    });
    run_test("development session staging",
             [&] { test_development_session_staging(helper, vendor); });
    run_test("development invalid credentials", [&] {
      test_development_session_rejects_invalid_credentials(helper, vendor);
    });
    run_test("development vendor digest mismatch", [&] {
      test_development_vendor_digest_mismatch_fails_before_staging(helper,
                                                                   vendor);
    });
    run_test("development helper digest mismatch", [&] {
      test_development_helper_digest_mismatch_fails_before_staging(stalled);
    });
    run_test("development launch cleanup", [&] {
      test_development_launch_failure_removes_directory(helper.parent_path());
    });
    run_test("development timeout cleanup",
             [&] { test_development_stalled_helper_times_out(stalled); });
    run_test("relative path rejection", test_relative_path_fails_closed);
    run_test("helper digest mismatch",
             [&] { test_digest_mismatch_fails_before_launch(stalled); });
    run_test("isolated launch cleanup", [&] {
      test_launch_failure_removes_profile(helper.parent_path());
    });
    run_test("isolated timeout cleanup",
             [&] { test_stalled_helper_times_out(stalled); });
    std::cout << "vikey_helper_process_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_process_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
