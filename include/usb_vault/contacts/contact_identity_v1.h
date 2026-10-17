#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace usb_vault::contacts {

inline constexpr std::size_t kContactPublicKeySize = 32;
inline constexpr std::size_t kContactSignatureSize = 64;
inline constexpr std::size_t kContactFingerprintSize = 10;
inline constexpr std::string_view kContactIdentityDomain =
    "usb-vault:contact-identity:v1";
inline constexpr std::string_view kContactKeyFileHeader =
    "USB-VAULT-CONTACT-KEY v1";
// Key files are a few dozen bytes; anything far larger is not a key file.
inline constexpr std::size_t kContactKeyFileMaxBytes = 4096;

// Ed25519 public key; the X25519 key used for encryption is converted from it.
using ContactPublicKey = std::array<std::byte, kContactPublicKeySize>;
using ContactX25519PublicKey = std::array<std::byte, kContactPublicKeySize>;
using ContactSignature = std::array<std::byte, kContactSignatureSize>;

// The card's contact identity, derived from the card root secret S so it is
// never stored: every login password and every backup card of the same S
// yields the same identity. Signing uses Ed25519 (SHA-512); key agreement uses
// X25519 with the secret converted from the same seed, as Monocypher documents
// for a single stored key. Secrets stay in page-locked memory and are cleared
// on destruction (R-03).
class ContactIdentity final {
 public:
  // card_secret must hold exactly 32 bytes; throws std::invalid_argument
  // otherwise.
  [[nodiscard]] static ContactIdentity derive(
      const security::SecureBuffer& card_secret);

  ContactIdentity(ContactIdentity&&) noexcept = default;
  ContactIdentity& operator=(ContactIdentity&&) noexcept = default;
  ContactIdentity(const ContactIdentity&) = delete;
  ContactIdentity& operator=(const ContactIdentity&) = delete;
  ~ContactIdentity() = default;

  [[nodiscard]] const ContactPublicKey& public_key() const noexcept {
    return public_key_;
  }
  [[nodiscard]] const ContactX25519PublicKey& x25519_public_key()
      const noexcept {
    return x25519_public_key_;
  }

  // Plain Ed25519 without context: every signed message must begin with its
  // own domain string. Both calls throw std::logic_error after a move.
  [[nodiscard]] ContactSignature sign(
      std::span<const std::byte> message) const;

  // Raw X25519 shared value with a peer; not uniformly random, so callers must
  // pass it through a KDF with both public keys. Returns nothing when the peer
  // key is low order (all-zero result).
  [[nodiscard]] std::optional<security::SecureBuffer> agree(
      const ContactX25519PublicKey& peer) const;

 private:
  ContactIdentity(security::SecureBuffer ed25519_secret,
                  security::SecureBuffer x25519_secret,
                  const ContactPublicKey& public_key,
                  const ContactX25519PublicKey& x25519_public_key) noexcept
      : ed25519_secret_(std::move(ed25519_secret)),
        x25519_secret_(std::move(x25519_secret)),
        public_key_(public_key),
        x25519_public_key_(x25519_public_key) {}

  security::SecureBuffer ed25519_secret_;
  security::SecureBuffer x25519_secret_;
  ContactPublicKey public_key_{};
  ContactX25519PublicKey x25519_public_key_{};
};

// Standard Ed25519 verification. Keys that are not usable (below) never
// verify; Monocypher additionally rejects s >= L.
[[nodiscard]] bool verify_contact_signature(
    const ContactPublicKey& public_key, std::span<const std::byte> message,
    const ContactSignature& signature) noexcept;

[[nodiscard]] ContactX25519PublicKey contact_x25519_public_key(
    const ContactPublicKey& public_key) noexcept;

// False for non-canonical encodings (y >= p) and for keys whose X25519 form is
// low order, with which no shared secret could be agreed. Such keys are
// rejected on import. A 32-byte value that is not on the curve is not
// detected here; it can be encrypted to (X25519 is twist secure) but never
// produces a signature that verifies.
[[nodiscard]] bool contact_public_key_usable(
    const ContactPublicKey& public_key) noexcept;

// First 10 bytes of SHA-256(domain || key), as 16 Crockford Base32 characters
// in groups of four ("XXXX-XXXX-XXXX-XXXX"), for comparing keys out of band.
[[nodiscard]] std::array<std::byte, kContactFingerprintSize>
contact_fingerprint(const ContactPublicKey& public_key);
[[nodiscard]] std::string format_contact_fingerprint(
    const ContactPublicKey& public_key);

// Share file: the header line, then Crockford Base32 of key || first 4 bytes
// of SHA-256(domain || key) in hyphen-separated groups of five. The file holds
// no name, card number, hardware ID or vault information.
[[nodiscard]] std::string encode_contact_key_file(
    const ContactPublicKey& public_key);

enum class ContactKeyFileError : std::uint8_t {
  none,
  malformed,
  unsupported_version,
  checksum_mismatch,
  unusable_key,
};

struct ContactKeyFileDecoding final {
  std::optional<ContactPublicKey> public_key;
  ContactKeyFileError error{ContactKeyFileError::malformed};
};

// Accepts an optional UTF-8 BOM, CRLF or LF line ends, case-insensitive
// symbols and any hyphens or blanks within the key line. A header naming
// another numeric version is unsupported_version; any other deviation is
// rejected with a more specific error or as malformed.
[[nodiscard]] ContactKeyFileDecoding decode_contact_key_file(
    std::string_view text);

}  // namespace usb_vault::contacts
