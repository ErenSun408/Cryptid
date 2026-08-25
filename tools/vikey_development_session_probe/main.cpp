#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_session.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>
#include <WinCred.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::Sha256Digest;
using usb_vault::security::SecureBuffer;

constexpr wchar_t kApiKeyCredentialTarget[] =
    L"usb-vault-vikey-dev-api-key";
constexpr wchar_t kUserSnCredentialTarget[] =
    L"usb-vault-vikey-dev-user-sn";
constexpr wchar_t kUserPinCredentialTarget[] =
    L"usb-vault-vikey-dev-user-pin";

static_assert(sizeof(wchar_t) == 2,
              "Windows credential decoding requires UTF-16 wchar_t");

[[noreturn]] void fail_windows(const char* const context) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), context);
}

class Credential final {
 public:
  explicit Credential(PCREDENTIALW value) noexcept : value_(value) {}
  ~Credential() noexcept {
    if (value_ != nullptr) {
      if (value_->CredentialBlob != nullptr &&
          value_->CredentialBlobSize != 0) {
        static_cast<void>(::SecureZeroMemory(value_->CredentialBlob,
                                             value_->CredentialBlobSize));
      }
      ::CredFree(value_);
    }
  }

  Credential(const Credential&) = delete;
  Credential& operator=(const Credential&) = delete;

  [[nodiscard]] const CREDENTIALW& get() const noexcept { return *value_; }

 private:
  PCREDENTIALW value_{};
};

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

[[nodiscard]] SecureBuffer read_development_credential(
    const wchar_t* const target, const std::size_t expected) {
  PCREDENTIALW raw = nullptr;
  if (::CredReadW(target, CRED_TYPE_GENERIC, 0, &raw) == FALSE ||
      raw == nullptr) {
    fail_windows("read development ViKey credential");
  }
  Credential credential(raw);
  const CREDENTIALW& value = credential.get();
  if (value.CredentialBlob == nullptr) {
    throw std::invalid_argument(
        "development ViKey credential has no value");
  }
  SecureBuffer result(expected);
  const bool raw_exact = value.CredentialBlobSize == expected;
  const bool raw_terminated =
      value.CredentialBlobSize == expected + 1 &&
      value.CredentialBlob[expected] == 0;
  if (raw_exact || raw_terminated) {
    for (std::size_t index = 0; index < expected; ++index) {
      result.bytes()[index] =
          static_cast<std::byte>(value.CredentialBlob[index]);
    }
    return result;
  }
  const std::size_t wide_bytes = expected * sizeof(wchar_t);
  const bool wide_exact = value.CredentialBlobSize == wide_bytes;
  const bool wide_terminated =
      value.CredentialBlobSize == wide_bytes + sizeof(wchar_t) &&
      value.CredentialBlob[wide_bytes] == 0 &&
      value.CredentialBlob[wide_bytes + 1] == 0;
  if (wide_exact || wide_terminated) {
    for (std::size_t index = 0; index < expected; ++index) {
      const std::size_t offset = index * sizeof(wchar_t);
      if (value.CredentialBlob[offset + 1] != 0) {
        throw std::invalid_argument(
            "development ViKey credential is not ASCII");
      }
      result.bytes()[index] =
          static_cast<std::byte>(value.CredentialBlob[offset]);
    }
    return result;
  }
  throw std::invalid_argument(
      "development ViKey credential has the wrong size");
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 5) {
      throw std::invalid_argument(
          "usage: probe <helper.exe> <helper-sha256> <vendor-dir> "
          "<vendor-sha256>");
    }
    const std::filesystem::path helper =
        std::filesystem::absolute(argv[1]).lexically_normal();
    const std::filesystem::path vendor =
        std::filesystem::absolute(argv[3]).lexically_normal();
    const Sha256Digest helper_digest = parse_digest(argv[2]);
    const Sha256Digest vendor_digest = parse_digest(argv[4]);

    SecureBuffer api_key = read_development_credential(
        kApiKeyCredentialTarget,
        usb_vault::card::vikey::kVikeyApiKeyBytes);
    SecureBuffer user_sn = read_development_credential(
        kUserSnCredentialTarget,
        usb_vault::card::vikey::kVikeyUserSnBytes);
    SecureBuffer user_pin = read_development_credential(
        kUserPinCredentialTarget,
        usb_vault::card::vikey::kVikeyUserPinBytes);

    const auto result = usb_vault::card::vikey::
        probe_vikey_development_session_with_network_access(
            helper, helper_digest, vendor, vendor_digest, user_sn, api_key,
            user_pin);
    std::cout << "ViKey development session passed; PIN attempts before="
              << static_cast<unsigned int>(result.attempts_before) << '/'
              << static_cast<unsigned int>(result.maximum_before)
              << " after="
              << static_cast<unsigned int>(result.attempts_after) << '/'
              << static_cast<unsigned int>(result.maximum_after) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ViKey development session failed: " << error.what()
              << '\n';
    return 1;
  }
}
