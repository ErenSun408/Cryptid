#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace usb_vault::licensing {

// One signed grant: the product may be used with the card whose ViKey
// hardware ID matches until the UTC instant, exclusive. The grant carries no
// password role, container, or tier information.
struct ActivationGrant final {
  std::uint32_t card_hardware_id{};
  std::int64_t expires_at_utc_seconds{};

  friend bool operator==(const ActivationGrant&,
                         const ActivationGrant&) = default;
};

// Uncompressed P-256 public key coordinates, X then Y, big-endian.
using ActivationPublicKey = std::array<std::byte, 64>;

// Signing key identifier carried in every code. The application accepts only
// the identifier its embedded public key was published under.
inline constexpr std::uint8_t kActivationKeyId = 1;

// Verification throws only when CNG itself fails; callers treat that as not
// activated. Code text: Crockford Base32 of version, key ID, hardware ID, expiry and a
// P-256 ECDSA signature, in hyphen-separated groups of five characters.
// Decoding ignores case, hyphens and whitespace; any other deviation rejects.
[[nodiscard]] std::optional<ActivationGrant> verify_activation_code(
    std::string_view code, const ActivationPublicKey& public_key);

// Holds an ECDSA P-256 signing key for the issuing tool. The private blob is
// the CNG BCRYPT_ECCPRIVATE_BLOB layout and stays in page-locked memory.
class ActivationSigner final {
 public:
  [[nodiscard]] static ActivationSigner generate();
  [[nodiscard]] static ActivationSigner import_private_blob(
      std::span<const std::byte> blob);

  ActivationSigner(ActivationSigner&&) noexcept = default;
  ActivationSigner& operator=(ActivationSigner&&) noexcept = default;
  ActivationSigner(const ActivationSigner&) = delete;
  ActivationSigner& operator=(const ActivationSigner&) = delete;
  ~ActivationSigner() = default;

  [[nodiscard]] const security::SecureBuffer& private_blob() const noexcept {
    return private_blob_;
  }
  [[nodiscard]] ActivationPublicKey public_key() const;
  [[nodiscard]] std::string sign(const ActivationGrant& grant) const;

 private:
  explicit ActivationSigner(security::SecureBuffer blob) noexcept
      : private_blob_(std::move(blob)) {}

  security::SecureBuffer private_blob_;
};

// Converts a wall-clock reading in a fixed UTC offset to UTC seconds since
// 1970-01-01. Offsets are minutes east of UTC within [-12:00, +14:00].
// Returns nothing for an invalid calendar date, time, or offset.
[[nodiscard]] std::optional<std::int64_t> utc_seconds_from_offset_time(
    int year, int month, int day, int hour, int minute, int second,
    int offset_minutes) noexcept;

enum class ActivationState : std::uint8_t {
  missing,
  active,
  expired,
  clock_rewound,
};

// Clock readings earlier than the highest one already observed by more than
// this tolerance are treated as a rewound clock rather than trusted.
inline constexpr std::int64_t kClockRewindToleranceSeconds = 10 * 60;

// Pure evaluation: no grant, or a grant for another card, is missing; a
// rewound or pre-1970 clock fails closed before the expiry comparison;
// otherwise expiry is exclusive. ECDSA signatures are malleable and the text
// tolerates aliases, so anything keyed on a code must use the decoded grant,
// never the code text.
[[nodiscard]] ActivationState evaluate_activation(
    const std::optional<ActivationGrant>& grant,
    std::uint32_t card_hardware_id, std::int64_t now_utc_seconds,
    std::int64_t highest_observed_utc_seconds) noexcept;

}  // namespace usb_vault::licensing
