#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <span>

namespace usb_vault::card::detail {

// Private crypto bridge: preserves v1's exact password/AAD domains during a
// root-preserving format upgrade. None of these operations accesses a card.
struct CardEnvelopeResult final {
  bool authenticated{false};
  security::SecureBuffer root_secret{32};
};

struct CardAeadResult final {
  bool authenticated{false};
  security::SecureBuffer plaintext;
};

void fill_card_random(std::span<std::byte> destination);
[[nodiscard]] security::SecureBuffer make_card_context_v1();
[[nodiscard]] security::SecureBuffer derive_card_master_v1(
    const security::SecureBuffer& password,
    std::span<const std::byte> context);
void encrypt_card_envelope_v1(std::span<const std::byte> context,
    std::byte slot, const security::SecureBuffer& root,
    const security::SecureBuffer& password, std::span<std::byte> destination);
[[nodiscard]] CardEnvelopeResult decrypt_card_envelope_v1(
    std::span<const std::byte> context, std::byte slot,
    const security::SecureBuffer& master,
    std::span<const std::byte> envelope);

[[nodiscard]] security::SecureBuffer derive_card_ring_metadata_key_v2(
    const security::SecureBuffer& root, std::span<const std::byte> context);
// Output is nonce(12), ciphertext(plaintext.size()), tag(16). Open always
// returns a locked output buffer and clears it on an authentication mismatch.
void seal_card_aead(const security::SecureBuffer& key,
    std::span<const std::byte> plaintext, std::span<const std::byte> aad,
    std::span<std::byte> destination);
[[nodiscard]] CardAeadResult open_card_aead(
    const security::SecureBuffer& key, std::span<const std::byte> envelope,
    std::span<const std::byte> aad);

} // namespace usb_vault::card::detail
