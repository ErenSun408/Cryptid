#pragma once

#include "card/vikey/vikey_helper_backend.h"

#include <cstddef>
#include <span>
#include <optional>

namespace usb_vault::card::vikey::development {

enum class CredentialField {
  user_sn,
  api_key,
  user_pin,
};

// Applies the same fixed development-card shape accepted by VikeySession:
// UserSN and API key are hexadecimal ASCII, while the PIN is alphanumeric
// ASCII. This performs no Credential Manager access.
[[nodiscard]] bool valid_credential_value(
    CredentialField field,
    std::span<const std::byte> value) noexcept;

// Decodes the byte layouts produced by Windows Credential Manager into locked
// memory. Exposed for deterministic tests; it does not access Credential
// Manager itself.
[[nodiscard]] security::SecureBuffer decode_credential_blob(
    std::span<const std::byte> credential_blob, std::size_t expected_bytes);

// Storage seam shared by the configurator and future in-app setup. A missing
// entry is nullopt; access errors must throw, never masquerade as absence.
enum class CredentialEntry { bundle, legacy_sn, legacy_key, legacy_pin };
class ICredentialStorage {
 public:
  virtual ~ICredentialStorage() = default;
  virtual std::optional<security::SecureBuffer> read(CredentialEntry entry) = 0;
  virtual void write_bundle(security::SecureBuffer& value) = 0;
};
[[nodiscard]] VikeyCredentialSet load_credentials(ICredentialStorage& storage);
void store_credentials(ICredentialStorage& storage, const VikeyCredentialSet& credentials);

// Development-only provider for the fixed ViKey credential targets on
// the current Windows user account. This is not a production credential
// provisioning decision and must not be used with production material.
class WindowsCredentialProvider final : public IVikeyCredentialProvider {
 public:
  [[nodiscard]] VikeyCredentialSet load_credentials() override;
};

// Validates all three values before replacing one versioned generic credential
// for the current Windows user, then verifies readback. Legacy entries are
// read only when the bundle is absent; reading never migrates or writes.
// Values are persisted only
// on this machine. This is development-only and is not a production secret
// provisioning boundary.
void store_credentials(VikeyCredentialSet credentials);

}  // namespace usb_vault::card::vikey::development
