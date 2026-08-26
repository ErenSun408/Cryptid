#include "card/vikey/vikey_development_credentials.h"

#include "card/vikey/vikey_session.h"

#include <Windows.h>
#include <WinCred.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace usb_vault::card::vikey::development {
namespace {

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
    if (value_ == nullptr) return;
    if (value_->CredentialBlob != nullptr &&
        value_->CredentialBlobSize != 0) {
      static_cast<void>(::SecureZeroMemory(value_->CredentialBlob,
                                           value_->CredentialBlobSize));
    }
    ::CredFree(value_);
  }

  Credential(const Credential&) = delete;
  Credential& operator=(const Credential&) = delete;
  Credential(Credential&&) = delete;
  Credential& operator=(Credential&&) = delete;

  [[nodiscard]] const CREDENTIALW& get() const noexcept { return *value_; }

 private:
  PCREDENTIALW value_{};
};

[[nodiscard]] security::SecureBuffer read_credential(
    const wchar_t* const target, const std::size_t expected_bytes) {
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
  return decode_credential_blob(
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(value.CredentialBlob),
          value.CredentialBlobSize),
      expected_bytes);
}

}  // namespace

security::SecureBuffer decode_credential_blob(
    const std::span<const std::byte> credential_blob,
    const std::size_t expected_bytes) {
  if (expected_bytes == 0 ||
      expected_bytes >
          ((std::numeric_limits<std::size_t>::max)() - sizeof(wchar_t)) /
              sizeof(wchar_t)) {
    throw std::invalid_argument(
        "development ViKey credential expected size is invalid");
  }

  security::SecureBuffer result(expected_bytes);
  const bool raw_exact = credential_blob.size() == expected_bytes;
  const bool raw_terminated =
      credential_blob.size() == expected_bytes + 1 &&
      credential_blob[expected_bytes] == std::byte{};
  if (raw_exact || raw_terminated) {
    std::ranges::copy_n(credential_blob.begin(), expected_bytes,
                        result.bytes().begin());
    return result;
  }

  const std::size_t wide_bytes = expected_bytes * sizeof(wchar_t);
  const bool wide_exact = credential_blob.size() == wide_bytes;
  const bool wide_terminated =
      credential_blob.size() == wide_bytes + sizeof(wchar_t) &&
      credential_blob[wide_bytes] == std::byte{} &&
      credential_blob[wide_bytes + 1] == std::byte{};
  if (wide_exact || wide_terminated) {
    for (std::size_t index = 0; index < expected_bytes; ++index) {
      const std::size_t offset = index * sizeof(wchar_t);
      if (credential_blob[offset + 1] != std::byte{}) {
        throw std::invalid_argument(
            "development ViKey credential is not ASCII");
      }
      result.bytes()[index] = credential_blob[offset];
    }
    return result;
  }

  throw std::invalid_argument(
      "development ViKey credential has the wrong size");
}

VikeyCredentialSet WindowsCredentialProvider::load_credentials() {
  return {
      .user_sn = read_credential(kUserSnCredentialTarget,
                                 kVikeyUserSnBytes),
      .api_key = read_credential(kApiKeyCredentialTarget,
                                 kVikeyApiKeyBytes),
      .user_pin = read_credential(kUserPinCredentialTarget,
                                  kVikeyUserPinBytes),
  };
}

}  // namespace usb_vault::card::vikey::development
