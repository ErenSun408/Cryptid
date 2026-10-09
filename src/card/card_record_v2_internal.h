#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace usb_vault::card::detail {

// The cursor remains private to the logical-record transaction implementation;
// the public authentication result exposes only the root, never slot identity.
struct AuthenticatedCardRecordV2 final {
  security::SecureBuffer root_secret;
  std::uint32_t next_slot{};
};

[[nodiscard]] AuthenticatedCardRecordV2 authenticate_card_record_v2(
    std::span<const std::byte> record, security::SecureBuffer password_utf8);
void seal_card_ring_metadata_v2(std::span<std::byte> record,
    const security::SecureBuffer& root, std::uint32_t next_slot);
// Same builder for all random initial positions; no semantic password roles.
[[nodiscard]] security::SecureBuffer build_initial_card_record_v2(
    std::span<const std::byte> context, const security::SecureBuffer& root,
    const security::SecureBuffer& password_utf8, std::uint32_t first_slot);
[[nodiscard]] std::uint32_t next_card_slot_v2(std::uint32_t slot);

} // namespace usb_vault::card::detail
