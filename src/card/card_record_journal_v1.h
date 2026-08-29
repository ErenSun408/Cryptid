#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <span>

namespace usb_vault::card {

inline constexpr std::size_t kCardRecordJournalV1DescriptorSize = 120;
inline constexpr std::size_t kCardRecordJournalV1Descriptor0Offset = 0;
inline constexpr std::size_t kCardRecordJournalV1Descriptor1Offset = 120;
inline constexpr std::size_t kCardRecordJournalV1Slot0Offset = 240;
inline constexpr std::size_t kCardRecordJournalV1Slot1Offset = 456;
inline constexpr std::size_t kCardRecordJournalV1StorageSize = 672;

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

// Reads both descriptors and both record slots before resolving the random
// commit-ID link. A torn descriptor can fall back during commit, but a complete
// newest descriptor whose referenced record is damaged fails closed instead
// of silently rolling back. Recoverable interrupted state is normalized before
// return by randomizing the inactive slot and mirroring selected metadata.
[[nodiscard]] security::SecureBuffer
read_card_record_journal_v1(ICardRecordJournalStorage &storage);

// Controlled provisioning entry point. Product initialization must not call
// this function: it accepts only the exact non-secret provisioning record,
// randomly selects its physical slot, and randomizes the inactive slot.
void provision_card_record_journal_v1(
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
