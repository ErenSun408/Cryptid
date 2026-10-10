#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_blob_backend.h"
#include "card/vikey/vikey_helper_card_enrollment.h"
#include "card/vikey/vikey_helper_card_record_backend.h"
#include "card/vikey/vikey_helper_protocol.h"
#include "vikey_network_probe.h"

#include "usb_vault/application/card_entry_router.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <Windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::CardEntryRouter;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::Sha256Digest;
using usb_vault::card::vikey::
    bootstrap_vikey_development_card_media_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_blob_round_trip_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_card_media_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_helper_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_session_with_network_access;
using usb_vault::card::vikey::
    probe_vikey_development_vendor_runtime_with_network_access;
using usb_vault::card::vikey::
    reset_vikey_development_legacy_media_with_network_access;
using usb_vault::card::vikey::probe_vikey_helper;
using usb_vault::card::vikey::probe_vikey_vendor_runtime;
using usb_vault::card::vikey::IVikeyCredentialProvider;
using usb_vault::card::vikey::VikeyCardRecordStore;
using usb_vault::card::vikey::VikeyCardStore;
using usb_vault::card::vikey::VikeyCredentialSet;
using usb_vault::card::vikey::VikeyHelperBlobBackend;
using usb_vault::card::vikey::VikeyHelperBackendConfig;
using usb_vault::card::vikey::VikeyHelperCardEnrollment;
using usb_vault::card::vikey::VikeyHelperCardInitializationTarget;
using usb_vault::card::vikey::VikeyHelperCardRecordBackend;
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

[[nodiscard]] bool current_process_is_elevated() {
  HANDLE token{};
  expect(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
      "cannot query parent elevation");
  TOKEN_ELEVATION elevation{};
  DWORD bytes{};
  const auto ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
  ::CloseHandle(token);
  expect(ok != FALSE && bytes == sizeof(elevation), "cannot read parent elevation");
  return elevation.TokenIsElevated != 0;
}

[[nodiscard]] std::uint32_t expected_development_security_properties() {
  using namespace usb_vault::card::vikey::helper;
  return kDevelopmentCompatibilitySecurityProperties |
      (current_process_is_elevated() ? 0u : property_bit(SecurityProperty::not_elevated));
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

void test_runtime_log_records_helper_calls(const std::filesystem::path& helper) {
#if USB_VAULT_DEBUG_LOG
  namespace diagnostics = usb_vault::diagnostics;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      (L"usb-vault-helper-log-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
       std::to_wstring(::GetTickCount64()));
  expect(std::filesystem::create_directory(root), "the helper log fixture already exists");
  const SYSTEMTIME time{2026, 9, 0, 26, 11, 0, 0, 0};
  const std::filesystem::path path = root / L"logs" / L"runtime-log-20260926-110000.txt";
  {
    diagnostics::DebugLogFile file;
    expect(file.open(root, time), "cannot open the helper log fixture");
    diagnostics::set_runtime_log(&file);
    struct Withdraw final {
      ~Withdraw() { usb_vault::diagnostics::set_runtime_log(nullptr); }
    } withdraw;
    static_cast<void>(probe_vikey_helper(helper, file_digest(helper), 5s));
    Sha256Digest wrong = file_digest(helper);
    wrong.front() ^= std::byte{1};
    try {
      static_cast<void>(probe_vikey_helper(helper, wrong, 5s));
      fail("a helper with the wrong digest was started");
    } catch (const std::system_error&) {
    }
  }
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    expect(in.good(), "the helper log was not written");
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  for (const char* const expected :
       {"[卡助手] 开始卡助手操作；操作 ", "边界 零网络 AppContainer", "[卡助手] 卡助手进程已创建；进程号 ",
        "[卡助手] 卡助手应答；状态 0；安全属性 0x", "[卡助手] 卡助手操作完成；",
        "[卡助手] 卡助手操作失败；", "异常类型："}) {
    if (text.find(expected) == std::string::npos) {
      fail((std::string("the helper log lacks: ") + expected).c_str());
    }
  }
  std::filesystem::remove(path);
  std::filesystem::remove(root / L"logs");
  std::filesystem::remove(root);
#else
  static_cast<void>(helper);
#endif
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
  // Verify both all remaining protections and the actual inherited elevation state.
  if (helper_result.security_properties !=
      expected_development_security_properties()) {
    std::cerr << "development helper security properties: actual=0x"
              << std::hex << helper_result.security_properties
              << " expected=0x"
              << expected_development_security_properties()
              << std::dec << '\n';
  }
  expect(helper_result.security_properties ==
             expected_development_security_properties(),
         "development helper did not retain its exact restricted boundary");
  expect(development_helper_directory_count() == directories_before,
         "development helper probe left a staging directory behind");
  expect(current_process_privileges() == parent_privileges_before,
         "development child privilege filtering modified the parent token");

  const auto network_result =
      probe_vikey_development_helper_with_network_access(
          network_enabled_fixture, file_digest(network_enabled_fixture), 5s);
  expect(network_result.security_properties ==
             expected_development_security_properties(),
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
             expected_development_security_properties(),
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

class CountingCredentialProvider final : public IVikeyCredentialProvider {
 public:
  [[nodiscard]] VikeyCredentialSet load_credentials() override {
    ++loads;
    if (fail_load) {
      throw std::runtime_error("fixture credential provider failed");
    }
    return {
        .user_sn = fixed_secret(
            usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'}),
        .api_key = fixed_secret(
            usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'}),
        .user_pin = fixed_secret(
            usb_vault::card::vikey::kVikeyUserPinBytes, pin_value),
    };
  }

  std::size_t loads{};
  bool fail_load{};
  std::byte pin_value{std::byte{'P'}};
};

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
             expected_development_security_properties(),
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

void test_development_blob_round_trip_staging(
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
  const auto test_blob = fixed_secret(
      usb_vault::card::vikey::kVikeyBlobBytes, std::byte{0xa5});

  const auto result =
      probe_vikey_development_blob_round_trip_with_network_access(
          helper, file_digest(helper), vendor.parent_path(),
          file_digest(vendor), user_sn, api_key, user_pin, test_blob, 5s);
  expect(result.security_properties ==
             expected_development_security_properties(),
         "development blob round trip lost a restricted-process property");
  expect_fixed_secret(test_blob, std::byte{0xa5},
                      "development blob round trip changed caller data");
  expect(development_helper_directory_count() == directories_before,
         "development blob round trip left a staging directory behind");

  const auto short_blob = fixed_secret(
      usb_vault::card::vikey::kVikeyBlobBytes - 1, std::byte{0xa5});
  try {
    static_cast<void>(
        probe_vikey_development_blob_round_trip_with_network_access(
            helper, file_digest(helper), vendor.parent_path(),
            file_digest(vendor), user_sn, api_key, user_pin, short_blob,
            5s));
    fail("short development test blob was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "short development test blob returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "short development test blob created a staging directory");
}

void store_u32(SecureBuffer& destination, const std::size_t offset,
               const std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    destination.bytes()[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] SecureBuffer initialized_record_fixture() {
  SecureBuffer record(usb_vault::card::kCardRecordV1Size);
  std::ranges::fill(record.bytes(), std::byte{0x7b});
  constexpr std::array<std::byte, 8> kMagic{
      std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
      std::byte{'D'}, std::byte{'2'}, std::byte{},    std::byte{}};
  std::ranges::copy(kMagic, record.bytes().begin());
  store_u32(record, 8, usb_vault::card::kCardRecordV1Version);
  store_u32(record, 12,
            static_cast<std::uint32_t>(usb_vault::card::kCardRecordV1Size));
  store_u32(record, 48, 512U * 1024U);
  store_u32(record, 52, 2);
  store_u32(record, 56, 1);
  store_u32(record, 60, 0x13);
  return record;
}

void test_formal_card_record_backend_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  VikeyHelperBackendConfig config{
      .helper_path = helper,
      .helper_digest = file_digest(helper),
      .vendor_directory = vendor.parent_path(),
      .vendor_digest = file_digest(vendor),
      .timeout = 5s,
  };
  const auto user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(
      usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'});
  const auto user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'P'});
  const auto media =
      probe_vikey_development_card_media_with_network_access(
          helper, config.helper_digest, vendor.parent_path(),
          config.vendor_digest, user_sn, api_key, user_pin, 5s);
  expect(media.hardware_id == 1,
         "card media probe returned the wrong hardware ID");
  expect(media.media_state ==
             usb_vault::card::vikey::VikeyCardMediaState::product_evidence,
         "card media probe returned the wrong media state");
  expect(media.logical_state ==
             usb_vault::card::CardRecordState::provisioned_uninitialized,
         "card media probe returned the wrong logical record state");
  expect(media.security_properties ==
             expected_development_security_properties(),
         "card media probe lost a restricted-process property");
  CountingCredentialProvider provider;
  VikeyHelperCardRecordBackend backend(config, provider,
                                       media.hardware_id);
  VikeyCardRecordStore store(backend);

  SecureBuffer observed = store.get_blob();
  expect(observed.size() == usb_vault::card::kCardRecordV1Size,
         "development card-record read returned the wrong size");
  expect(usb_vault::card::inspect_card_record(observed.bytes()) ==
             usb_vault::card::CardRecordState::provisioned_uninitialized,
         "development card-record read returned the wrong journal record");

  SecureBuffer record = initialized_record_fixture();
  const std::vector<std::byte> original(record.bytes().begin(),
                                        record.bytes().end());
  store.put_blob(record);
  expect(std::ranges::equal(record.bytes(), original),
         "development card-record write changed caller data");
  // A three-envelope record travels and commits through the same single
  // helper operation; the in-process journal commit verifies it.
  SecureBuffer three_slot(usb_vault::card::kCardRecordV1ThreeSlotSize);
  std::ranges::fill(three_slot.bytes(), std::byte{0x7c});
  std::ranges::copy(record.bytes().first(usb_vault::card::kCardRecordV1HeaderSize),
                    three_slot.bytes().begin());
  store.put_blob(three_slot);
  const auto initialized_user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'R'});
  const auto initialized_media =
      probe_vikey_development_card_media_with_network_access(
          helper, config.helper_digest, vendor.parent_path(),
          config.vendor_digest, user_sn, api_key, initialized_user_pin, 5s);
  expect(initialized_media.hardware_id == media.hardware_id &&
             initialized_media.media_state ==
                 usb_vault::card::vikey::
                     VikeyCardMediaState::product_evidence &&
             initialized_media.logical_state ==
                 usb_vault::card::CardRecordState::initialized_v1,
         "card media probe did not classify the initialized fixture record");

  auto snapshot = usb_vault::card::vikey::read_vikey_development_card_record_with_network_access(
      helper, config.helper_digest, vendor.parent_path(), config.vendor_digest,
      media.hardware_id, user_sn, api_key, initialized_user_pin, 5s, true);
  expect(usb_vault::card::inspect_card_record(snapshot.bytes()) ==
             usb_vault::card::CardRecordState::initialized_v1,
         "reset preparation did not return the initialized snapshot");
  usb_vault::card::vikey::write_vikey_development_card_record_with_network_access(
      helper, config.helper_digest, vendor.parent_path(), config.vendor_digest,
      media.hardware_id, user_sn, api_key, initialized_user_pin, snapshot, 5s, true);
  // Fixture PIN R recreates a deterministic initialized card per helper process.
  // A changed expected record must therefore fail before any reset write.
  snapshot.bytes().back() ^= std::byte{1};
  bool stale_rejected = false;
  try {
    usb_vault::card::vikey::write_vikey_development_card_record_with_network_access(
        helper, config.helper_digest, vendor.parent_path(), config.vendor_digest,
        media.hardware_id, user_sn, api_key, initialized_user_pin, snapshot, 5s, true);
  } catch (const std::system_error&) { stale_rejected = true; }
  expect(stale_rejected, "helper reset accepted a stale record snapshot");

  SecureBuffer short_record(usb_vault::card::kCardRecordV1Size - 1);
  try {
    store.put_blob(short_record);
    fail("short development card record was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "short development card record returned the wrong error");
  }
  expect(provider.loads == 3,
         "formal card-record backend cached or reused credentials");
  expect(development_helper_directory_count() == directories_before,
         "development card-record operations left staging residue");

  CountingCredentialProvider mismatched_provider;
  VikeyHelperCardRecordBackend mismatched_backend(
      config, mismatched_provider, media.hardware_id + 1U);
  VikeyCardRecordStore mismatched_store(mismatched_backend);
  try {
    static_cast<void>(mismatched_store.get_blob());
    fail("mismatched card-record hardware identity was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "mismatched card-record identity returned the wrong error");
  }
  expect(mismatched_provider.loads == 1,
         "mismatched card-record identity retried credentials");
  expect(development_helper_directory_count() == directories_before,
         "mismatched card-record identity left staging residue");

  try {
    VikeyHelperCardRecordBackend zero_identity_backend(config, provider, 0);
    static_cast<void>(zero_identity_backend);
    fail("zero card-record hardware identity was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "zero card-record identity returned the wrong error");
  }

  CountingCredentialProvider failing_provider;
  failing_provider.fail_load = true;
  VikeyHelperCardRecordBackend failing_backend(std::move(config),
                                                failing_provider,
                                                media.hardware_id);
  VikeyCardRecordStore failing_store(failing_backend);
  try {
    static_cast<void>(failing_store.get_blob());
    fail("card-record credential provider failure was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "card-record credential failure returned the wrong error");
  }
  expect(failing_provider.loads == 1,
         "card-record credential provider failure was retried");
  expect(development_helper_directory_count() == directories_before,
         "card-record credential failure staged a helper directory");
}

void test_development_card_media_bootstrap_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(
      usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'});
  const auto blank_user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'Q'});
  const auto helper_digest = file_digest(helper);
  const auto vendor_digest = file_digest(vendor);

  bootstrap_vikey_development_card_media_with_network_access(
      helper, helper_digest, vendor.parent_path(), vendor_digest, 1,
      usb_vault::card::vikey::VikeyCardMediaState::blank, user_sn,
      api_key, blank_user_pin, 5s);
  expect(development_helper_directory_count() == directories_before,
         "card media bootstrap left staging residue");

  try {
    bootstrap_vikey_development_card_media_with_network_access(
        helper, helper_digest, vendor.parent_path(), vendor_digest, 2,
        usb_vault::card::vikey::VikeyCardMediaState::blank, user_sn,
        api_key, blank_user_pin, 5s);
    fail("mismatched card media bootstrap identity was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "mismatched bootstrap identity returned the wrong error");
  }

  try {
    bootstrap_vikey_development_card_media_with_network_access(
        helper, helper_digest, vendor.parent_path(), vendor_digest, 1,
        usb_vault::card::vikey::
            VikeyCardMediaState::provisioning_interrupted,
        user_sn, api_key, blank_user_pin, 5s);
    fail("changed card media bootstrap state was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "changed bootstrap state returned the wrong error");
  }

  try {
    bootstrap_vikey_development_card_media_with_network_access(
        helper, helper_digest, vendor.parent_path(), vendor_digest, 1,
        usb_vault::card::vikey::VikeyCardMediaState::foreign_data,
        user_sn, api_key, blank_user_pin, 5s);
    fail("ineligible card media bootstrap state was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "ineligible bootstrap state returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "failed card media bootstrap left staging residue");
}

void test_development_legacy_media_reset_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  const auto user_sn = fixed_secret(
      usb_vault::card::vikey::kVikeyUserSnBytes, std::byte{'A'});
  const auto api_key = fixed_secret(
      usb_vault::card::vikey::kVikeyApiKeyBytes, std::byte{'B'});
  const auto legacy_user_pin = fixed_secret(
      usb_vault::card::vikey::kVikeyUserPinBytes, std::byte{'S'});
  const auto helper_digest = file_digest(helper);
  const auto vendor_digest = file_digest(vendor);

  const auto media = probe_vikey_development_card_media_with_network_access(
      helper, helper_digest, vendor.parent_path(), vendor_digest,
      user_sn, api_key, legacy_user_pin, 5s);
  expect(media.hardware_id == 1 &&
             media.media_state == usb_vault::card::vikey::
                                      VikeyCardMediaState::
                                          legacy_p1_or_foreign_prefix,
         "development reset fixture did not expose P1-shaped media");

  reset_vikey_development_legacy_media_with_network_access(
      helper, helper_digest, vendor.parent_path(), vendor_digest,
      media.hardware_id, media.media_state, user_sn, api_key,
      legacy_user_pin, 5s);
  expect(development_helper_directory_count() == directories_before,
         "development media reset left staging residue");

  try {
    reset_vikey_development_legacy_media_with_network_access(
        helper, helper_digest, vendor.parent_path(), vendor_digest, 2,
        media.media_state, user_sn, api_key, legacy_user_pin, 5s);
    fail("mismatched development reset identity was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "mismatched development reset identity returned the wrong error");
  }

  try {
    reset_vikey_development_legacy_media_with_network_access(
        helper, helper_digest, vendor.parent_path(), vendor_digest, 1,
        usb_vault::card::vikey::VikeyCardMediaState::foreign_data,
        user_sn, api_key, legacy_user_pin, 5s);
    fail("foreign development reset state was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "foreign development reset state returned the wrong error");
  }
  expect(development_helper_directory_count() == directories_before,
         "failed development media reset left staging residue");
}

void test_formal_card_enrollment_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  VikeyHelperBackendConfig config{
      .helper_path = helper,
      .helper_digest = file_digest(helper),
      .vendor_directory = vendor.parent_path(),
      .vendor_digest = file_digest(vendor),
      .timeout = 5s,
  };
  CountingCredentialProvider provider;
  provider.pin_value = std::byte{'Q'};
  VikeyHelperCardEnrollment enrollment(config, provider);

  auto inspection = enrollment.inspect_inserted_card();
  auto* const insertion = dynamic_cast<
      usb_vault::card::vikey::VikeyCardInsertionContext*>(
      inspection.get());
  if (insertion == nullptr) {
    fail("formal enrollment source returned the wrong context type");
  }
  const auto& context = insertion->probe();
  expect(context.hardware_id == 1 &&
             context.media_state ==
                 usb_vault::card::vikey::VikeyCardMediaState::blank &&
             !context.logical_state.has_value(),
         "formal enrollment returned the wrong insertion context");
  expect(provider.loads == 1,
         "formal enrollment probe did not load one credential set");
  expect(CardEntryRouter::route(*inspection) ==
             CardEntryRoute::initialization_required,
         "formal enrollment insertion context reached the wrong route");
  expect(provider.loads == 1,
         "formal insertion routing started another helper operation");
  VikeyHelperCardInitializationTarget target(config, provider, context);
  target.ensure_provisioning_record();
  expect(provider.loads == 2,
         "formal enrollment bootstrap reused probe credentials");
  target.ensure_provisioning_record();
  expect(provider.loads == 2,
         "formal initialization target repeated verified bootstrap");
  expect(!target.card_store().capabilities().supports(
             usb_vault::card::CardCapability::multiple_pins),
         "formal initialization target exposed unexpected capabilities");

  auto zero_identity = context;
  zero_identity.hardware_id = 0;
  try {
    enrollment.bootstrap(zero_identity);
    fail("formal enrollment accepted a zero hardware identity");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "formal enrollment zero identity returned the wrong error");
  }
  expect(provider.loads == 2,
         "formal enrollment loaded credentials for a zero identity");

  auto ineligible = context;
  ineligible.media_state =
      usb_vault::card::vikey::VikeyCardMediaState::foreign_data;
  try {
    enrollment.bootstrap(ineligible);
    fail("formal enrollment accepted foreign media");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "formal enrollment foreign media returned the wrong error");
  }
  expect(provider.loads == 2,
         "formal enrollment loaded credentials for foreign media");

  auto contradictory = context;
  contradictory.logical_state = CardRecordState::initialized_v1;
  try {
    enrollment.bootstrap(contradictory);
    fail("formal enrollment accepted contradictory logical state");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "contradictory enrollment context returned the wrong error");
  }
  expect(provider.loads == 2,
         "formal enrollment loaded credentials for contradictory context");
  try {
    VikeyHelperCardInitializationTarget contradictory_target(
        config, provider, contradictory);
    static_cast<void>(contradictory_target);
    fail("formal initialization target accepted contradictory state");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_configuration),
           "contradictory initialization target returned the wrong error");
  }
  expect(provider.loads == 2,
         "formal target loaded credentials for contradictory context");

  auto stale_context = context;
  stale_context.media_state =
      usb_vault::card::vikey::VikeyCardMediaState::
          provisioning_interrupted;
  VikeyHelperCardInitializationTarget stale_target(
      config, provider, stale_context);
  try {
    stale_target.ensure_provisioning_record();
    fail("formal initialization target accepted a changed media state");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "formal initialization target returned the wrong state error");
  }
  expect(provider.loads == 3,
         "formal initialization target did not make one bootstrap attempt");
  try {
    stale_target.ensure_provisioning_record();
    fail("formal initialization target retried an uncertain failure");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::invalid_operation),
           "stale initialization target returned the wrong retry error");
  }
  expect(provider.loads == 3,
         "stale initialization target reloaded credentials");

  CountingCredentialProvider failing_provider;
  failing_provider.fail_load = true;
  VikeyHelperCardEnrollment failing_enrollment(std::move(config),
                                                failing_provider);
  try {
    static_cast<void>(failing_enrollment.inspect_entry());
    fail("formal enrollment accepted a credential-provider failure");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "formal enrollment credential failure returned the wrong error");
  }
  expect(failing_provider.loads == 1,
         "formal enrollment retried failed credentials");
  expect(development_helper_directory_count() == directories_before,
         "formal enrollment left staging residue");
}

void test_formal_blob_backend_staging(
    const std::filesystem::path& helper,
    const std::filesystem::path& vendor) {
  const std::size_t directories_before =
      development_helper_directory_count();
  VikeyHelperBackendConfig config{
      .helper_path = helper,
      .helper_digest = file_digest(helper),
      .vendor_directory = vendor.parent_path(),
      .vendor_digest = file_digest(vendor),
      .timeout = 5s,
  };
  CountingCredentialProvider provider;
  VikeyHelperBlobBackend backend(config, provider);
  VikeyCardStore store(backend);

  SecureBuffer observed = store.get_blob();
  expect_fixed_secret(observed, std::byte{0x3c},
                      "formal blob backend returned the wrong data");

  SecureBuffer blob = fixed_secret(
      usb_vault::card::vikey::kVikeyBlobBytes, std::byte{0xd5});
  store.put_blob(blob);
  expect_fixed_secret(blob, std::byte{0xd5},
                      "formal blob backend changed caller data");

  SecureBuffer partial_failure = fixed_secret(
      usb_vault::card::vikey::kVikeyBlobBytes, std::byte{0xe1});
  try {
    store.put_blob(partial_failure);
    fail("partial formal blob write was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::device_failure),
           "partial formal blob write returned the wrong error");
  }
  expect_fixed_secret(partial_failure, std::byte{0xe1},
                      "failed formal blob write changed caller data");
  expect(provider.loads == 3,
         "formal blob backend cached or reused credentials");
  expect(development_helper_directory_count() == directories_before,
         "formal blob backend left a staging directory behind");

  CountingCredentialProvider failing_provider;
  failing_provider.fail_load = true;
  VikeyHelperBlobBackend failing_backend(std::move(config),
                                         failing_provider);
  VikeyCardStore failing_store(failing_backend);
  try {
    static_cast<void>(failing_store.get_blob());
    fail("credential provider failure was accepted");
  } catch (const std::system_error& error) {
    expect(error.code() == usb_vault::card::make_error_code(
                               CardError::backend_unavailable),
           "credential provider failure returned the wrong error");
  }
  expect(failing_provider.loads == 1,
         "credential provider failure was retried");
  expect(development_helper_directory_count() == directories_before,
         "credential provider failure staged a helper directory");
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
                               CardError::operation_timed_out),
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
                               CardError::operation_timed_out),
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
    const bool elevated_development = argc == 7 && std::wstring_view(argv[6]) == L"--elevated-development";
    if (argc != 6 && !elevated_development) {
      fail("expected helper, isolated-network, network-enabled, stalled, "
           "and vendor fixture paths");
    }
    expect(current_process_is_elevated() == elevated_development,
        "test mode does not match actual parent elevation");
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
    if (!elevated_development) {
      run_test("restricted helper probe",
               [&] { test_restricted_helper_probe(helper); });
      run_test("runtime log records helper calls",
               [&] { test_runtime_log_records_helper_calls(helper); });
      run_test("vendor runtime staging",
               [&] { test_vendor_runtime_staging(helper, vendor); });
      run_test("vendor digest mismatch", [&] {
        test_vendor_digest_mismatch_fails_before_launch(helper, vendor);
      });
      run_test("network isolation", [&] { test_network_isolation(network); });
    }
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
    run_test("development blob round trip", [&] {
      test_development_blob_round_trip_staging(helper, vendor);
    });
    run_test("formal card-record backend", [&] {
      test_formal_card_record_backend_staging(helper, vendor);
    });
    run_test("development card media bootstrap", [&] {
      test_development_card_media_bootstrap_staging(helper, vendor);
    });
    run_test("development legacy media reset", [&] {
      test_development_legacy_media_reset_staging(helper, vendor);
    });
    run_test("formal card enrollment", [&] {
      test_formal_card_enrollment_staging(helper, vendor);
    });
    run_test("formal blob backend", [&] {
      test_formal_blob_backend_staging(helper, vendor);
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
    if (!elevated_development) {
      run_test("isolated launch cleanup", [&] {
        test_launch_failure_removes_profile(helper.parent_path());
      });
      run_test("isolated timeout cleanup",
               [&] { test_stalled_helper_times_out(stalled); });
    }
    std::cout << "vikey_helper_process_tests passed; elevated_development=" << elevated_development << "\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_helper_process_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
