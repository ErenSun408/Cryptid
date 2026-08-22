#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace usb_vault::security {

inline constexpr std::uint32_t kKdfV1Version = 1;
inline constexpr std::uint32_t kKdfV1MemoryKib = 512U * 1024U;
inline constexpr std::uint32_t kKdfV1Iterations = 2;
inline constexpr std::uint32_t kKdfV1Parallelism = 1;
inline constexpr std::size_t kKdfV1CardSecretSize = 32;
inline constexpr std::size_t kKdfV1SaltSize = 16;
inline constexpr std::size_t kKdfV1OutputSize = 64;
inline constexpr std::string_view kKdfV1DomainTag =
    "usb-vault:volume-password:v1";

using KdfV1Salt = std::array<std::byte, kKdfV1SaltSize>;

// Derives the raw VeraCrypt volume password with Argon2id v19. The password
// buffer must contain a non-empty, well-formed UTF-8 byte sequence. Its bytes
// are consumed exactly as supplied; this function performs no Unicode
// normalization. card_secret must contain exactly 32 bytes.
//
// Password, card secret, and output remain in page-locked SecureBuffer
// allocations. Argon2's fixed 512 MiB work area is a separate private
// VirtualAlloc allocation that is deliberately pageable for predictable
// operation on ordinary Windows systems and is wiped before release.
[[nodiscard]] SecureBuffer derive_volume_password_v1(
    const SecureBuffer& password_utf8,
    const SecureBuffer& card_secret,
    const KdfV1Salt& salt);

}  // namespace usb_vault::security
