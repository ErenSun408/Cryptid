#pragma once

#include "card/vikey/vikey_helper_blob_backend.h"

#include <cstddef>
#include <span>

namespace usb_vault::card::vikey::development {

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

}  // namespace usb_vault::card::vikey::development
