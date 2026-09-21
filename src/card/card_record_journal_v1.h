#pragma once

#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <span>

namespace usb_vault::card {

inline constexpr std::size_t kCardRecordJournalV1DescriptorSize = 152;
inline constexpr std::size_t kCardRecordJournalV1Descriptor0Offset = 0;
inline constexpr std::size_t kCardRecordJournalV1Descriptor1Offset = 152;
inline constexpr std::size_t kCardRecordJournalV1Slot0Offset = 304;
inline constexpr std::size_t kCardRecordJournalV1Slot1Offset = 520;
inline constexpr std::size_t kCardRecordJournalV1StorageSize = 736;

static_assert(kCardRecordJournalV1Descriptor1Offset ==
              kCardRecordJournalV1Descriptor0Offset +
                  kCardRecordJournalV1DescriptorSize);
static_assert(kCardRecordJournalV1Slot0Offset ==
              kCardRecordJournalV1Descriptor1Offset +
                  kCardRecordJournalV1DescriptorSize);
static_assert(kCardRecordJournalV1Slot1Offset ==
              kCardRecordJournalV1Slot0Offset + 216);
static_assert(kCardRecordJournalV1StorageSize ==
              kCardRecordJournalV1Slot1Offset + 216);

// Read-only evidence visible in the reserved journal range. None is not by
// itself authorization to erase or adopt a card: a hardware adapter must also
// prove that the target media is accessible, stable, and contains no legacy
// or foreign data according to that adapter's contract.
enum class CardRecordJournalMediaState {
  no_product_evidence,
  provisioning_interrupted,
  product_evidence,
};

// Vendor-private random-access boundary used to make the application's single
// logical card record recoverable. Implementations must either return exactly
// the requested byte count or fail; partial writes may occur before failure.
class ICardRecordJournalStorage {
public:
  virtual ~ICardRecordJournalStorage() noexcept = default;

  ICardRecordJournalStorage(const ICardRecordJournalStorage &) = delete;
  ICardRecordJournalStorage &
  operator=(const ICardRecordJournalStorage &) = delete;
  ICardRecordJournalStorage(ICardRecordJournalStorage &&) = delete;
  ICardRecordJournalStorage &operator=(ICardRecordJournalStorage &&) = delete;

  [[nodiscard]] virtual std::size_t capacity() = 0;
  [[nodiscard]] virtual security::SecureBuffer read(std::size_t offset,
                                                    std::size_t length) = 0;
  virtual void write(std::size_t offset, std::span<const std::byte> bytes) = 0;

protected:
  ICardRecordJournalStorage() noexcept = default;
};

// Snapshot and conditional destructive reset used only after native confirmation.
[[nodiscard]] security::SecureBuffer snapshot_card_record_journal_v1(ICardRecordJournalStorage& storage);
void reset_card_record_journal_v1(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected);

// Reads both descriptors and both record slots before resolving the random
// commit-ID link. A torn descriptor can fall back during commit, but a complete
// newest descriptor whose referenced record is damaged fails closed instead
// of silently rolling back. Recoverable interrupted state is normalized before
// return by randomizing the inactive slot and mirroring selected metadata.
[[nodiscard]] security::SecureBuffer
read_card_record_journal_v1(ICardRecordJournalStorage &storage);

// Reads the complete reserved range without normalizing or writing it.
// Recognizable current/future product prefixes and valid descriptors are
// product evidence even when damaged. An exact complete provisioning marker
// without a valid descriptor is the only resumable bootstrap state.
[[nodiscard]] CardRecordJournalMediaState
inspect_card_record_journal_media_v1(
    ICardRecordJournalStorage &storage);

// Selects the authoritative logical record using the same descriptor and
// commit-chain validation as a normal journal read, then classifies only its
// public framing. This operation never repairs, normalizes, or writes storage,
// even when the selected commit is recoverable but not yet mirrored. Missing,
// ambiguous, or damaged active commits fail instead of becoming malformed or
// uninitialized product records.
[[nodiscard]] CardRecordState
inspect_card_record_journal_logical_state_v1_read_only(
    ICardRecordJournalStorage &storage);

// Low-level controlled provisioning primitive. It accepts only the exact
// non-secret provisioning record, refuses existing product evidence, randomly
// selects its physical slot, and randomizes the inactive slot. Product code
// uses the idempotent bootstrap wrapper below after adapter authorization.
void provision_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    std::span<const std::byte> provisioning_record);

// Idempotent product bootstrap. It may start only after the adapter has
// authorized no-product-evidence media, resume an interrupted provisioning
// marker, or accept an already recoverable provisioning journal. Existing,
// damaged, initialized, and unsupported product records are never replaced.
void bootstrap_uninitialized_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    std::span<const std::byte> provisioning_record);

// Atomically replaces the logical record by writing and verifying the inactive
// slot, publishing a new descriptor, randomizing the old slot, and mirroring
// metadata. A write error is accepted only when readback proves the intended
// bytes landed.
void commit_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    std::span<const std::byte> replacement_record);

} // namespace usb_vault::card
