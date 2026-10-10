#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace usb_vault::card {

inline constexpr std::uint32_t kCardRecordV1Version = 1;
inline constexpr std::size_t kCardRecordV1Size = 216;
inline constexpr std::size_t kCardRecordV1HeaderSize = 64;
inline constexpr std::size_t kCardRecordV1EnvelopeSize = 76;
inline constexpr std::size_t kCardRecordV1PrimaryEnvelopeOffset = 64;
inline constexpr std::size_t kCardRecordV1SecondaryEnvelopeOffset = 140;
inline constexpr std::size_t kCardRecordV1RootSecretSize = 32;
inline constexpr std::size_t kLegacyP1CardBlobSize = 32;
// The same v1 header followed by a third envelope. The header keeps its v1
// length field; the journal descriptor carries the real record length.
inline constexpr std::size_t kCardRecordV1TertiaryEnvelopeOffset = 216;
inline constexpr std::size_t kCardRecordV1ThreeSlotSize = 292;

enum class CardRecordState {
  provisioned_uninitialized,
  initialized_v1,
  legacy_p1_blob,
  unsupported_version,
  malformed,
};

// Wrong passwords and damaged envelope authentication are intentionally
// indistinguishable. Neither condition may be converted into an initialization
// path that overwrites the card.
class CardCredentialRejected final : public std::runtime_error {
public:
  CardCredentialRejected();
};

struct InitializedCardRecordV1 final {
  security::SecureBuffer record;
  security::SecureBuffer root_secret;
};

// Classifies public framing only and never attempts a password operation.
// A card is initializable only when it contains the exact provisioning marker;
// blank-looking, truncated, foreign, and legacy values fail closed.
[[nodiscard]] CardRecordState
inspect_card_record(std::span<const std::byte> record) noexcept;

// Produces the exact non-secret marker written by the controlled card
// provisioning process before a user initializes the product.
[[nodiscard]] security::SecureBuffer provision_uninitialized_card_record_v1();

// Creates a fixed-size record with one encrypted primary envelope and a
// random, indistinguishable secondary envelope. The password is consumed and
// wiped; the generated root secret only leaves this function encrypted.
[[nodiscard]] security::SecureBuffer
initialize_card_record_v1(security::SecureBuffer password_utf8);

// Initialization workflow variant that returns the generated root secret in
// locked memory so a successfully persisted card can immediately enter an
// authenticated no-volume session without repeating the password KDF.
// The password is consumed and wiped.
[[nodiscard]] InitializedCardRecordV1
initialize_card_record_and_root_v1(
    security::SecureBuffer password_utf8);

// Accepts a two- or three-envelope record. After one password-hardening
// operation every envelope is tried under every slot binding, because a kept
// envelope may have been moved; all matches must hold the same root. Only that
// root is returned: slot identity must not become persistent application state.
[[nodiscard]] security::SecureBuffer
unlock_card_record_v1(std::span<const std::byte> record,
                      security::SecureBuffer password_utf8);

// Re-encrypts the existing root into both envelopes. This generic operation
// stores no semantic slot label or activation field.
// Existing and replacement passwords are consumed and wiped.
[[nodiscard]] security::SecureBuffer
rewrap_card_record_v1(std::span<const std::byte> record,
                      security::SecureBuffer existing_password_utf8,
                      security::SecureBuffer primary_password_utf8,
                      security::SecureBuffer secondary_password_utf8);

// v1's [new, existing] rewrap extended by one slot. The result is always the
// three-envelope record [new, existing, kept]: kept is the old first envelope,
// or the old second one when the existing password opened the first. The
// header and the kept envelope's ciphertext are preserved byte for byte, so a
// two-envelope record expands in place. A new password equal to the existing
// one is rejected. Both passwords are consumed and wiped.
[[nodiscard]] security::SecureBuffer
append_card_password_v1(std::span<const std::byte> record,
                        security::SecureBuffer existing_password_utf8,
                        security::SecureBuffer new_password_utf8);

} // namespace usb_vault::card
