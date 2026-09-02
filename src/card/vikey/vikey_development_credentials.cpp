#include "card/vikey/vikey_development_credentials.h"

#include "card/vikey/vikey_session.h"

#include <Windows.h>
#include <WinCred.h>

#include <algorithm>
#include <limits>
#include <ranges>
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
constexpr wchar_t kCredentialUserName[] =
    L"USB Vault ViKey development";

static_assert(sizeof(wchar_t) == 2,
              "Windows credential decoding requires UTF-16 wchar_t");

[[noreturn]] void fail_windows(const char* const context) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), context);
}

[[nodiscard]] bool ascii_hex(const std::byte value) noexcept {
  const unsigned int byte = std::to_integer<unsigned int>(value);
  return (byte >= static_cast<unsigned int>('0') &&
          byte <= static_cast<unsigned int>('9')) ||
         (byte >= static_cast<unsigned int>('A') &&
          byte <= static_cast<unsigned int>('F')) ||
         (byte >= static_cast<unsigned int>('a') &&
          byte <= static_cast<unsigned int>('f'));
}

[[nodiscard]] bool ascii_alphanumeric(const std::byte value) noexcept {
  const unsigned int byte = std::to_integer<unsigned int>(value);
  return (byte >= static_cast<unsigned int>('0') &&
          byte <= static_cast<unsigned int>('9')) ||
         (byte >= static_cast<unsigned int>('A') &&
          byte <= static_cast<unsigned int>('Z')) ||
         (byte >= static_cast<unsigned int>('a') &&
          byte <= static_cast<unsigned int>('z'));
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

void write_credential(const wchar_t* const target,
                      security::SecureBuffer& value) {
  static_assert(kVikeyApiKeyBytes <= CRED_MAX_CREDENTIAL_BLOB_SIZE);
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = const_cast<wchar_t*>(target);
  credential.CredentialBlobSize = static_cast<DWORD>(value.size());
  credential.CredentialBlob =
      reinterpret_cast<LPBYTE>(value.data());
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.UserName = const_cast<wchar_t*>(kCredentialUserName);
  if (::CredWriteW(&credential, 0) == FALSE) {
    fail_windows("write development ViKey credential");
  }
}

}  // namespace

bool valid_credential_value(
    const CredentialField field,
    const std::span<const std::byte> value) noexcept {
  switch (field) {
    case CredentialField::user_sn:
      return value.size() == kVikeyUserSnBytes &&
             std::ranges::all_of(value, ascii_hex);
    case CredentialField::api_key:
      return value.size() == kVikeyApiKeyBytes &&
             std::ranges::all_of(value, ascii_hex);
    case CredentialField::user_pin:
      return value.size() == kVikeyUserPinBytes &&
             std::ranges::all_of(value, ascii_alphanumeric);
  }
  return false;
}

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

void store_credentials(VikeyCredentialSet credentials) {
  if (!valid_credential_value(CredentialField::user_sn,
                              credentials.user_sn.bytes()) ||
      !valid_credential_value(CredentialField::api_key,
                              credentials.api_key.bytes()) ||
      !valid_credential_value(CredentialField::user_pin,
                              credentials.user_pin.bytes())) {
    throw std::invalid_argument(
        "development ViKey credentials have an invalid shape");
  }

  // All three shapes are checked before this point. CredWriteW has no
  // multi-item transaction, so a system failure can still leave a prefix of
  // the set updated; the configuration UI tells the operator to save all
  // three fields again rather than treating a partial result as usable.
  write_credential(kUserSnCredentialTarget, credentials.user_sn);
  write_credential(kApiKeyCredentialTarget, credentials.api_key);
  write_credential(kUserPinCredentialTarget, credentials.user_pin);

  WindowsCredentialProvider provider;
  VikeyCredentialSet stored = provider.load_credentials();
  if (!std::ranges::equal(credentials.user_sn.bytes(),
                          stored.user_sn.bytes()) ||
      !std::ranges::equal(credentials.api_key.bytes(),
                          stored.api_key.bytes()) ||
      !std::ranges::equal(credentials.user_pin.bytes(),
                          stored.user_pin.bytes())) {
    throw std::system_error(
        static_cast<int>(ERROR_WRITE_FAULT), std::system_category(),
        "verify development ViKey credentials");
  }
}

}  // namespace usb_vault::card::vikey::development
