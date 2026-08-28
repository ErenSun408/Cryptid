#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace usb_vault::security::detail {

struct Argon2idParameters final {
  std::uint32_t memory_kib{};
  std::uint32_t iterations{};
  std::uint32_t parallelism{};
};

// Runs the repository's single Argon2id work area. Password validation,
// allocation, wiping, and serialization are shared by every KDF domain so two
// callers cannot accidentally reserve independent 512 MiB arenas at once.
[[nodiscard]] SecureBuffer derive_argon2id(const SecureBuffer &password_utf8,
                                           std::span<const std::byte> salt,
                                           std::span<const std::byte> secret,
                                           std::string_view domain_tag,
                                           Argon2idParameters parameters,
                                           std::size_t output_size);

} // namespace usb_vault::security::detail
