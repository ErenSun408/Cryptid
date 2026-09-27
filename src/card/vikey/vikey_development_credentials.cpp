#include "card/diagnostic_error.h"
#include "usb_vault/diagnostics/debug_log.h"
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

[[nodiscard]] std::optional<security::SecureBuffer> read_raw_credential(
    const wchar_t* target) {
  PCREDENTIALW raw = nullptr;
  if (::CredReadW(target, CRED_TYPE_GENERIC, 0, &raw) == FALSE) {
    if (::GetLastError() == ERROR_NOT_FOUND) return std::nullopt;
    fail_windows("read development ViKey credential");
  }
  Credential credential(raw);
  if (raw == nullptr || raw->CredentialBlob == nullptr ||
      raw->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    throw std::invalid_argument("invalid credential blob");
  security::SecureBuffer result(raw->CredentialBlobSize);
  std::ranges::copy(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(raw->CredentialBlob), raw->CredentialBlobSize),
      result.bytes().begin());
  return result;
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

namespace {
constexpr std::size_t kBundleHeaderSize = 4;
constexpr std::size_t kBundleSize = kBundleHeaderSize + kVikeyUserSnBytes + kVikeyApiKeyBytes + kVikeyUserPinBytes;
constexpr std::byte kBundleHeader[] = {std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{1}};
static_assert(kBundleSize <= CRED_MAX_CREDENTIAL_BLOB_SIZE);

class WindowsCredentialStorage final : public ICredentialStorage {
 public:
  std::optional<security::SecureBuffer> read(CredentialEntry entry) override {
    switch (entry) {
      case CredentialEntry::bundle: return read_raw_credential(L"usb-vault-vikey-dev-credentials");
      case CredentialEntry::legacy_sn: return read_raw_credential(kUserSnCredentialTarget);
      case CredentialEntry::legacy_key: return read_raw_credential(kApiKeyCredentialTarget);
      case CredentialEntry::legacy_pin: return read_raw_credential(kUserPinCredentialTarget);
    }
    throw std::invalid_argument("unknown credential entry");
  }
  void write_bundle(security::SecureBuffer& value) override {
    write_credential(L"usb-vault-vikey-dev-credentials", value);
  }
};

security::SecureBuffer encode_bundle(const VikeyCredentialSet& credentials) {
  if (!valid_credential_value(CredentialField::user_sn, credentials.user_sn.bytes()) ||
      !valid_credential_value(CredentialField::api_key, credentials.api_key.bytes()) ||
      !valid_credential_value(CredentialField::user_pin, credentials.user_pin.bytes()))
    throw std::invalid_argument("development ViKey credentials have an invalid shape");
  security::SecureBuffer result(kBundleSize);
  auto output = std::ranges::copy(kBundleHeader, result.bytes().begin()).out;
  output = std::ranges::copy(credentials.user_sn.bytes(), output).out;
  output = std::ranges::copy(credentials.api_key.bytes(), output).out;
  std::ranges::copy(credentials.user_pin.bytes(), output);
  return result;
}
}

VikeyCredentialSet load_credentials(ICredentialStorage& storage) {
  std::optional<security::SecureBuffer> bundle;
  try { bundle = storage.read(CredentialEntry::bundle); }
  catch (const std::system_error& e) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_development_credentials", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::credential_sn,
        "read saved credentials", static_cast<std::uint32_t>(e.code().value()), NativeCodeKind::win32);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_development_credentials", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    throw DiagnosticError(make_error_code(CardError::invalid_configuration), DiagnosticStage::credential_sn,
        "invalid credential bundle");
  }
  if (bundle && (bundle->size() != kBundleSize ||
      !std::ranges::equal(bundle->bytes().first(kBundleHeaderSize), kBundleHeader)))
    throw DiagnosticError(make_error_code(CardError::invalid_configuration), DiagnosticStage::credential_sn,
        "invalid credential bundle version or size");

  std::size_t offset = kBundleHeaderSize;
  const auto load = [&](CredentialEntry entry, std::size_t bytes, CredentialField field,
                        DiagnosticStage stage) {
    try {
      security::SecureBuffer value(bytes);
      if (bundle) {
        std::ranges::copy(bundle->bytes().subspan(offset, bytes), value.bytes().begin());
        offset += bytes;
      } else {
        auto legacy = storage.read(entry);
        if (!legacy) throw std::system_error(ERROR_NOT_FOUND, std::system_category());
        value = decode_credential_blob(legacy->bytes(), bytes);
      }
      if (!valid_credential_value(field, value.bytes()))
        throw std::invalid_argument("credential format invalid");
      return value;
    } catch (const std::system_error& e) {
      USB_VAULT_TRACE_FAILURE("card/vikey/vikey_development_credentials", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), stage,
          "read saved credential", static_cast<std::uint32_t>(e.code().value()), NativeCodeKind::win32);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("card/vikey/vikey_development_credentials", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      throw DiagnosticError(make_error_code(CardError::invalid_configuration), stage,
          "saved credential format invalid");
    }
  };
  return {
    .user_sn = load(CredentialEntry::legacy_sn, kVikeyUserSnBytes, CredentialField::user_sn, DiagnosticStage::credential_sn),
    .api_key = load(CredentialEntry::legacy_key, kVikeyApiKeyBytes, CredentialField::api_key, DiagnosticStage::credential_key),
    .user_pin = load(CredentialEntry::legacy_pin, kVikeyUserPinBytes, CredentialField::user_pin, DiagnosticStage::credential_pin),
  };
}

void store_credentials(ICredentialStorage& storage, const VikeyCredentialSet& credentials) {
  auto bundle = encode_bundle(credentials);
  storage.write_bundle(bundle);
  auto stored = storage.read(CredentialEntry::bundle);
  if (!stored || !std::ranges::equal(bundle.bytes(), stored->bytes()))
    throw std::system_error(ERROR_WRITE_FAULT, std::system_category(), "verify development ViKey credentials");
}

VikeyCredentialSet WindowsCredentialProvider::load_credentials() {
  WindowsCredentialStorage storage;
  return development::load_credentials(storage);
}

void store_credentials(VikeyCredentialSet credentials) {
  WindowsCredentialStorage storage;
  store_credentials(storage, credentials);
}

}  // namespace usb_vault::card::vikey::development