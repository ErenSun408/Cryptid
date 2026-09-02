#pragma once

#include "card/vikey/vikey_helper_backend.h"

#include <cstddef>
#include <span>

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

// Development-only provider for the three fixed ViKey credential targets on
// the current Windows user account. This is not a production credential
// provisioning decision and must not be used with production material.
class WindowsCredentialProvider final : public IVikeyCredentialProvider {
 public:
  [[nodiscard]] VikeyCredentialSet load_credentials() override;
};

// Validates all three values before the first write, then replaces the fixed
// generic credentials for the current Windows user. Values are persisted only
// on this machine. This is development-only and is not a production secret
// provisioning boundary.
void store_credentials(VikeyCredentialSet credentials);

}  // namespace usb_vault::card::vikey::development
