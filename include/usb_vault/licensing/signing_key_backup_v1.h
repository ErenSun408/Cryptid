#pragma once

#include "usb_vault/licensing/activation_code_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace usb_vault::licensing {

// Portable backup of the activation signing key. The issuing machine keeps
// its working copy under DPAPI, which cannot leave that Windows account; this
// file can, so losing the machine does not strand every distributed copy of
// the application. Layout: magic, Argon2id salt, AES-256-GCM nonce, sealed
// private blob and tag. The magic is authenticated as associated data.
inline constexpr std::size_t kSigningKeyBackupBytes = 8 + 16 + 12 + 104 + 16;

// The password must be non-empty, well-formed UTF-8.
[[nodiscard]] std::vector<std::byte> seal_signing_key_backup(
    const ActivationSigner& signer, const security::SecureBuffer& password_utf8);

// Returns nothing for a wrong password or any altered byte; throws only for a
// malformed password buffer or a CNG failure.
[[nodiscard]] std::optional<ActivationSigner> open_signing_key_backup(
    std::span<const std::byte> backup,
    const security::SecureBuffer& password_utf8);

}  // namespace usb_vault::licensing
