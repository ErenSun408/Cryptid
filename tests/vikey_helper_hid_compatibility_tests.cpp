#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_protocol.h"
#include "card/vikey/vikey_hid_probe.h"
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
#include <vector>

namespace {

using namespace std::chrono_literals;
using usb_vault::card::CardError;
using usb_vault::card::Sha256Digest;

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}
std::uint32_t expected_development_security_properties() {
  HANDLE token{};
  expect(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
      "cannot query parent elevation");
  TOKEN_ELEVATION elevation{};
  DWORD bytes{};
  const auto ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
  CloseHandle(token);
  expect(ok && bytes == sizeof(elevation), "cannot read parent elevation");
  using namespace usb_vault::card::vikey::helper;
  return kDevelopmentCompatibilitySecurityProperties |
      (elevation.TokenIsElevated ? 0u : property_bit(SecurityProperty::not_elevated));
}

[[nodiscard]] Sha256Digest file_digest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const std::streamoff size = input.tellg();
  expect(input.good() && size > 0 &&
             static_cast<unsigned long long>(size) <=
                 std::numeric_limits<ULONG>::max(),
         "could not size the helper artifact");
  std::vector<UCHAR> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(size));
  expect(input.good(), "could not read the helper artifact");

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  expect(BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
             &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)),
         "could not open SHA-256 provider");
  Sha256Digest digest{};
  const NTSTATUS status = ::BCryptHash(
      algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
      reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()));
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));
  expect(BCRYPT_SUCCESS(status), "could not hash the helper artifact");
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

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 2) fail("expected the helper path");
    if (current_process_token_is_restricted()) {
      std::cout << "vikey_helper_hid_compatibility_tests skipped: parent "
                   "token is already restricted\n";
      return 77;
    }

    const auto host_probe =
        usb_vault::card::vikey::probe_vikey_hid_access();
    if (!host_probe.device_present) {
      std::cout << "vikey_helper_hid_compatibility_tests skipped: ViKeyPRO "
                   "is not present\n";
      return 77;
    }
    if (!host_probe.read_write_opened) {
      std::cerr << "host ViKeyPRO open failed with Windows error "
                << host_probe.open_error << '\n';
      fail("unisolated HID positive control could not open ViKeyPRO");
    }

    const std::filesystem::path helper =
        std::filesystem::absolute(argv[1]).lexically_normal();
    const std::size_t profiles_before = helper_profile_count();
    const std::size_t directories_before =
        development_helper_directory_count();
    usb_vault::card::vikey::VikeyHelperProbe result{};
    try {
      result = usb_vault::card::vikey::probe_vikey_hid_access(
          helper, file_digest(helper), 5s);
    } catch (const std::system_error& error) {
      expect(helper_profile_count() == profiles_before,
             "denied HID probe left an AppContainer profile behind");
      if (error.code() == usb_vault::card::make_error_code(
                              CardError::permission_denied)) {
        std::cout << "zero-capability AppContainer could not open ViKeyPRO "
                     "as expected (Windows ERROR_ACCESS_DENIED)\n";
      } else {
        throw;
      }
    }
    if (result.security_properties != 0) {
      expect(result.security_properties ==
                 usb_vault::card::vikey::helper::
                     kRequiredSecurityProperties,
             "isolated HID probe lost a required security property");
    }
    expect(helper_profile_count() == profiles_before,
           "HID probe left an AppContainer profile behind");

    const auto development_result =
        usb_vault::card::vikey::
            probe_vikey_development_hid_access_with_network_access(
                helper, file_digest(helper), 5s);
    // Compatibility mode reports actual parent elevation, while all other
    // required properties must still match exactly.
    if (development_result.security_properties !=
        expected_development_security_properties()) {
      std::cerr << "development HID security properties: actual=0x"
                << std::hex << development_result.security_properties
                << " expected=0x"
                << expected_development_security_properties()
                << std::dec << '\n';
    }
    expect(development_result.security_properties ==
               expected_development_security_properties(),
           "development HID probe lost a restricted-process property");
    expect(development_helper_directory_count() == directories_before,
           "development HID probe left a staging directory behind");
    std::cout << "vikey_helper_hid_compatibility_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_hid_compatibility_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
