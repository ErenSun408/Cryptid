#pragma once

#include "usb_vault/contacts/contact_identity_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::contacts {

// Card user-area range owned by the contact list (P5.14). Bytes below belong
// to the card record journal reserved range; bytes from the end onward are
// never touched.
inline constexpr std::size_t kContactStoreV1RegionOffset = 1808;
inline constexpr std::size_t kContactStoreV1RegionEnd = 8192;
inline constexpr std::size_t kContactStoreV1RegionSize =
    kContactStoreV1RegionEnd - kContactStoreV1RegionOffset;
inline constexpr std::size_t kContactStoreV1CopySize =
    kContactStoreV1RegionSize / 2;
inline constexpr std::size_t kContactStoreV1CopyOffsets[2] = {
    kContactStoreV1RegionOffset,
    kContactStoreV1RegionOffset + kContactStoreV1CopySize};
inline constexpr std::size_t kContactStoreV1MaxContacts = 48;
inline constexpr std::size_t kContactRemarkMaxBytes = 31;

static_assert(kContactStoreV1RegionOffset == 1808);
static_assert(kContactStoreV1CopySize == 3192);
static_assert(kContactStoreV1CopyOffsets[1] + kContactStoreV1CopySize ==
              kContactStoreV1RegionEnd);

// Random access to the card user area, addressed from its start.
// Implementations return exactly the requested byte count or throw; a write
// may be partial before it throws.
class IContactStoreStorage {
 public:
  virtual ~IContactStoreStorage() noexcept = default;
  IContactStoreStorage(const IContactStoreStorage&) = delete;
  IContactStoreStorage& operator=(const IContactStoreStorage&) = delete;

  [[nodiscard]] virtual std::size_t capacity() = 0;
  [[nodiscard]] virtual security::SecureBuffer read(std::size_t offset,
                                                    std::size_t length) = 0;
  virtual void write(std::size_t offset, std::span<const std::byte> bytes) = 0;

 protected:
  IContactStoreStorage() noexcept = default;
};

// Thrown by an IContactStoreStorage read when the card user area is too small
// for the region; load() reports it as unavailable.
class ContactStoreCapacityError final : public std::runtime_error {
 public:
  ContactStoreCapacityError()
      : std::runtime_error("card user area is too small for the contact list") {}
};

struct ContactEntry final {
  ContactPublicKey public_key{};
  std::string remark;  // Validated UTF-8, 1..31 bytes.

  friend bool operator==(const ContactEntry&, const ContactEntry&) = default;
};

enum class ContactRemarkError : std::uint8_t {
  none,
  empty,
  too_long,
  invalid_text,
};

struct ContactRemarkNormalization final {
  std::optional<std::string> remark;
  ContactRemarkError error{ContactRemarkError::invalid_text};
};

// Trims Unicode white space at both ends, then requires well-formed UTF-8 of
// 1..31 bytes without control, invisible, filler, format-direction, variation
// selector or noncharacter code points. Nothing is truncated: an overlong
// remark is rejected. No Unicode normalization is applied. These rules are
// part of format v1: stored remarks must satisfy them, so tightening them
// later requires a new format version rather than a silent change.
[[nodiscard]] ContactRemarkNormalization normalize_contact_remark(
    std::string_view remark);

enum class ContactEntriesError : std::uint8_t {
  none,
  too_many,
  invalid_remark,
  unusable_key,
  duplicate_key,
};

// Validates a complete list as stored: remarks must already be normalized.
[[nodiscard]] ContactEntriesError validate_contact_entries(
    std::span<const ContactEntry> entries);

enum class ContactStoreState : std::uint8_t {
  // Not read yet, or the last read or write left the card state unknown.
  unread,
  // Card user area smaller than kContactStoreV1RegionEnd.
  unavailable,
  // Neither copy authenticates: blank, foreign, or another root secret.
  empty,
  loaded,
  // A copy authenticates with a known format but its body or entries do not,
  // and no intact copy exists.
  damaged,
  // A copy authenticates but names a format or layout this build does not
  // know. Never read as empty and never written.
  unsupported_version,
  // Two intact copies carry the same generation with different contents.
  inconsistent,
};

// Thrown by save and discard_and_clear when the card region no longer matches
// what load() read (another card with the same S, another writer, or a read
// error during load). Nothing was written; the store has been reloaded.
class ContactStoreChangedError final : public std::runtime_error {
 public:
  ContactStoreChangedError()
      : std::runtime_error("contact store changed since it was loaded") {}
};

// The authenticated contact list on the card: two copies, each written
// whole, the older copy replaced on every save and read back before success.
// An interrupted save loses only that save. Copies are indistinguishable from
// random bytes. The key is derived from S, so every login password and every
// backup card of the same S reads the same list.
class ContactStoreV1 final {
 public:
  // card_secret must hold exactly 32 bytes. The storage must outlive this
  // object; nothing is read until load().
  ContactStoreV1(IContactStoreStorage& storage,
                 const security::SecureBuffer& card_secret);

  ContactStoreV1(const ContactStoreV1&) = delete;
  ContactStoreV1& operator=(const ContactStoreV1&) = delete;

  ContactStoreState load();

  [[nodiscard]] ContactStoreState state() const noexcept { return state_; }
  // Entries of the loaded list; empty in every other state.
  [[nodiscard]] const std::vector<ContactEntry>& entries() const noexcept {
    return entries_;
  }

  // Allowed only in the empty and loaded states. Before writing, the whole
  // region is read again and must equal what load() read; after writing, the
  // whole region is resolved again and the new copy must be the one selected.
  // Throws std::invalid_argument for a list validate_contact_entries rejects,
  // std::logic_error for another state, ContactStoreChangedError when the
  // region changed since load(), and std::runtime_error when the new copy is
  // not selected on read-back. Storage exceptions propagate. After a failure
  // that reached the card the state is reloaded (unread if that fails too).
  void save(std::span<const ContactEntry> entries);

  // Explicit recovery for the damaged and inconsistent states: writes an
  // empty list newer than every authenticated copy. Same failure rules.
  void discard_and_clear();

 private:
  void write_copy(std::span<const ContactEntry> entries);
  void reload_after_failure() noexcept;

  IContactStoreStorage& storage_;
  security::SecureBuffer key_;
  ContactStoreState state_{ContactStoreState::unread};
  std::vector<ContactEntry> entries_;
  // Highest generation of any copy whose descriptor authenticated.
  std::uint64_t highest_generation_{0};
  // Copy index the next save writes.
  std::size_t next_copy_{0};
  // Ciphertext of the whole region as last read or written; compared before
  // every write so a stale view never overwrites newer copies.
  std::vector<std::byte> region_;
};

}  // namespace usb_vault::contacts
