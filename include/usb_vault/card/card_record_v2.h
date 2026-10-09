#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace usb_vault::card {

inline constexpr std::uint32_t kCardRecordV2Version = 2;
inline constexpr std::size_t kCardRecordV2Size = 384;
inline constexpr std::size_t kCardRecordV2HeaderSize = 32;
inline constexpr std::size_t kCardRecordV2ContextOffset = 32;
inline constexpr std::size_t kCardRecordV2ContextSize = 64;
inline constexpr std::size_t kCardRecordV2SlotCount = 3;
inline constexpr std::size_t kCardRecordV2EnvelopeOffset = 96;
inline constexpr std::size_t kCardRecordV2EnvelopeSize = 76;
inline constexpr std::size_t kCardRecordV2MetadataOffset = 324;
inline constexpr std::size_t kCardRecordV2MetadataSize = 60;
inline constexpr std::size_t kCardRecordV2RootSecretSize = 32;

enum class CardRecordV2State {
  initialized,
  unsupported_version,
  malformed,
};

struct InitializedCardRecordV2 final {
  security::SecureBuffer record;
  security::SecureBuffer root_secret;
};

// Pure logical-record operations. They neither access hardware nor authorize
// migration, initialization or overwriting an existing card. Password ownership
// is consumed and wiped, including when an exception is raised.
[[nodiscard]] CardRecordV2State inspect_card_record_v2(
    std::span<const std::byte> record) noexcept;
[[nodiscard]] InitializedCardRecordV2 initialize_card_record_and_root_v2(
    security::SecureBuffer password_utf8);
[[nodiscard]] security::SecureBuffer unlock_card_record_v2(
    std::span<const std::byte> record, security::SecureBuffer password_utf8);
// Preserve the old context and both encrypted envelopes without asking for the
// other password. Physical migration/confirmation is a separate transaction.
[[nodiscard]] security::SecureBuffer upgrade_card_record_v1_to_v2(
    std::span<const std::byte> record, security::SecureBuffer password_utf8);
// Blindly replace the encrypted cursor's target; never authenticate the new
// password against any previous envelope. The two other envelopes stay intact.
[[nodiscard]] security::SecureBuffer append_card_password_v2(
    std::span<const std::byte> record,
    security::SecureBuffer existing_password_utf8,
    security::SecureBuffer new_password_utf8);

} // namespace usb_vault::card
