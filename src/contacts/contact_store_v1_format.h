#pragma once

#include "usb_vault/contacts/contact_store_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Private copy encoding shared by ContactStoreV1 and its tests.
//
// Forward compatibility: this build refuses a later format only if that
// format still places a descriptor at kContactStoreV1RegionOffset with the
// same store key, a 12-byte nonce, exactly 36 bytes of plaintext, a 16-byte
// tag, and the descriptor AAD (domain, copy offset, region length 6384) of
// v1, with format version and layout in its first three bytes. A descriptor
// of any other shape would not authenticate here, the region would read as
// empty, and the next save would overwrite it.
namespace usb_vault::contacts::detail {

inline constexpr std::size_t kNonceSize = 12;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kDescriptorPlainSize = 36;
inline constexpr std::size_t kDescriptorSize =
    kNonceSize + kDescriptorPlainSize + kTagSize;
inline constexpr std::size_t kBodySize =
    kContactStoreV1CopySize - kDescriptorSize;
inline constexpr std::size_t kBodyPlainSize = kBodySize - kNonceSize - kTagSize;
inline constexpr std::size_t kEntrySize = 64;
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::uint8_t kLayoutTwoCopies = 1;
// Reserved for a future single-copy layout (descriptor at the first copy
// offset); this build reports it as unsupported_version.
inline constexpr std::uint8_t kLayoutSingleCopy = 2;

static_assert(kDescriptorSize == 64);
static_assert(kBodyPlainSize == 3100);
static_assert(kEntrySize * kContactStoreV1MaxContacts <= kBodyPlainSize);
static_assert(kEntrySize == kContactPublicKeySize + 1 + kContactRemarkMaxBytes);

// Descriptor plaintext: version u16, layout u8, copy u8, generation u64,
// entry count u16, body plaintext size u16, then 20 reserved zero bytes.
inline constexpr std::size_t kDescriptorReservedOffset = 16;

struct CopyHeader final {
  std::uint16_t format_version{kFormatVersion};
  std::uint8_t layout{kLayoutTwoCopies};
  std::uint8_t copy_index{};
  std::uint64_t generation{};
  std::uint16_t entry_count{};
  std::uint16_t body_plain_size{static_cast<std::uint16_t>(kBodyPlainSize)};
  // Tests only: nonzero puts a byte into the reserved area.
  std::uint8_t reserved_probe{};
};

using Copy = std::array<std::byte, kContactStoreV1CopySize>;

[[nodiscard]] security::SecureBuffer derive_store_key(
    const security::SecureBuffer& card_secret);

// Encodes entries into a body plaintext; entries must already be valid.
[[nodiscard]] security::SecureBuffer encode_body(
    std::span<const ContactEntry> entries);

// Seals a copy for the given card offset. body_plain must be kBodyPlainSize.
[[nodiscard]] Copy seal_copy(const security::SecureBuffer& key,
                             std::size_t copy_offset, const CopyHeader& header,
                             std::span<const std::byte> body_plain);

enum class CopyStatus : std::uint8_t {
  invalid,      // Descriptor does not authenticate.
  unsupported,  // Descriptor authenticates; unknown version or layout.
  damaged,      // Known descriptor; body or entries do not verify.
  intact,
};

struct OpenedCopy final {
  CopyStatus status{CopyStatus::invalid};
  std::uint64_t generation{};  // Set whenever the descriptor authenticated.
  std::vector<ContactEntry> entries;
};

[[nodiscard]] OpenedCopy open_copy(const security::SecureBuffer& key,
                                   std::size_t copy_offset,
                                   std::span<const std::byte> copy);

struct Resolution final {
  ContactStoreState state{ContactStoreState::empty};
  std::vector<ContactEntry> entries;
  std::uint64_t highest_generation{};
  std::uint64_t selected_generation{};
  std::size_t next_copy{};
};

// Selects the authoritative list from a whole region read.
[[nodiscard]] Resolution resolve_region(const security::SecureBuffer& key,
                                        std::span<const std::byte> region);

}  // namespace usb_vault::contacts::detail
