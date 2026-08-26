#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_blob_backend.h"
#include "card/vikey/vikey_session.h"
#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
#include "random_test_blob.h"
#endif
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::Sha256Digest;
using usb_vault::security::SecureBuffer;

[[nodiscard]] unsigned int hex_nibble(const wchar_t value) {
  if (value >= L'0' && value <= L'9') {
    return static_cast<unsigned int>(value - L'0');
  }
  if (value >= L'A' && value <= L'F') {
    return static_cast<unsigned int>(value - L'A' + 10);
  }
  if (value >= L'a' && value <= L'f') {
    return static_cast<unsigned int>(value - L'a' + 10);
  }
  throw std::invalid_argument("SHA-256 contains a non-hexadecimal character");
}

[[nodiscard]] Sha256Digest parse_digest(const wchar_t* const value) {
  if (value == nullptr) throw std::invalid_argument("missing SHA-256");
  std::size_t length = 0;
  while (value[length] != L'\0') ++length;
  if (length != 64) {
    throw std::invalid_argument("SHA-256 must contain exactly 64 hex digits");
  }
  Sha256Digest result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    const unsigned int high = hex_nibble(value[index * 2]);
    const unsigned int low = hex_nibble(value[index * 2 + 1]);
    result[index] = static_cast<std::byte>((high << 4U) | low);
  }
  return result;
}

#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
constexpr std::wstring_view kRiskAcceptance =
    L"--i-accept-card-interruption-risk";
#endif

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
    if (argc != 6 || argv[5] == nullptr ||
        std::wstring_view(argv[5]) != kRiskAcceptance) {
      throw std::invalid_argument(
          "usage: probe <helper.exe> <helper-sha256> <vendor-dir> "
          "<vendor-sha256> --i-accept-card-interruption-risk");
    }
#else
    if (argc != 5) {
      throw std::invalid_argument(
          "usage: probe <helper.exe> <helper-sha256> <vendor-dir> "
          "<vendor-sha256>");
    }
#endif
    const std::filesystem::path helper =
        std::filesystem::absolute(argv[1]).lexically_normal();
    const std::filesystem::path vendor =
        std::filesystem::absolute(argv[3]).lexically_normal();
    const Sha256Digest helper_digest = parse_digest(argv[2]);
    const Sha256Digest vendor_digest = parse_digest(argv[4]);

#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
    SecureBuffer test_blob =
        usb_vault::card::vikey::development::random_test_blob();
#endif
    usb_vault::card::vikey::development::WindowsCredentialProvider
        credential_provider;

#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
    auto credentials = credential_provider.load_credentials();
    static_cast<void>(
        usb_vault::card::vikey::
            probe_vikey_development_blob_round_trip_with_network_access(
                helper, helper_digest, vendor, vendor_digest,
                credentials.user_sn, credentials.api_key,
                credentials.user_pin, test_blob));
    std::cout << "ViKey development blob round trip passed; original blob "
                 "restoration was verified\n";
#elif defined(USB_VAULT_VIKEY_FORMAL_BLOB_READ_PROBE)
    usb_vault::card::vikey::VikeyHelperBlobBackend backend(
        {.helper_path = helper,
         .helper_digest = helper_digest,
         .vendor_directory = vendor,
         .vendor_digest = vendor_digest},
        credential_provider);
    usb_vault::card::vikey::VikeyCardStore store(backend);
    SecureBuffer blob = store.get_blob();
    if (blob.size() != usb_vault::card::vikey::kVikeyBlobBytes) {
      throw std::runtime_error(
          "formal ViKey backend returned an invalid blob size");
    }
    std::cout << "Formal ViKey development blob read passed; a 32-byte blob "
                 "was returned in locked memory\n";
#else
    auto credentials = credential_provider.load_credentials();
    const auto result = usb_vault::card::vikey::
        probe_vikey_development_session_with_network_access(
            helper, helper_digest, vendor, vendor_digest,
            credentials.user_sn, credentials.api_key,
            credentials.user_pin);
    std::cout << "ViKey development session passed; PIN attempts before="
              << static_cast<unsigned int>(result.attempts_before) << '/'
              << static_cast<unsigned int>(result.maximum_before)
              << " after="
              << static_cast<unsigned int>(result.attempts_after) << '/'
              << static_cast<unsigned int>(result.maximum_after) << '\n';
#endif
    return 0;
  } catch (const std::exception& error) {
#if defined(USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE)
    std::cerr << "ViKey development blob round trip failed: " << error.what()
              << '\n';
#elif defined(USB_VAULT_VIKEY_FORMAL_BLOB_READ_PROBE)
    std::cerr << "Formal ViKey development blob read failed: " << error.what()
              << '\n';
#else
    std::cerr << "ViKey development session failed: " << error.what()
              << '\n';
#endif
    return 1;
  }
}
