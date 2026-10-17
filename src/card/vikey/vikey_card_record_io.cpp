#include "card/vikey/vikey_card_record_io.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "card/card_record_journal_v1.h"
#include "card/vikey/vikey_session.h"
#include "card/diagnostic_error.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/contacts/contact_store_v1.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <system_error>

namespace usb_vault::card::vikey {
namespace {

constexpr std::size_t kMediaScanChunkSize = 4096;
constexpr std::byte kBlankUserDataByte{};

}  // namespace

class VikeyCardRecordStorage final : public ICardRecordJournalStorage {
 public:
  explicit VikeyCardRecordStorage(VikeySession& session) noexcept
      : session_(session) {}

  [[nodiscard]] std::size_t capacity() override {
    if (!capacity_) capacity_ = session_.user_data_capacity();
    return *capacity_;
  }

  [[nodiscard]] security::SecureBuffer read(const std::size_t offset,
                                            const std::size_t length) override {
    return session_.read_user_data(offset, length);
  }

  void write(const std::size_t offset,
             const std::span<const std::byte> bytes) override {
    session_.write_user_data(offset, bytes);
  }

 private:
  VikeySession& session_;
  std::optional<std::size_t> capacity_;
};

namespace {

[[nodiscard]] bool range_is_blank(ICardRecordJournalStorage& storage,
                                  const std::size_t begin,
                                  const std::size_t end) {
  for (std::size_t offset = begin; offset < end;) {
    const std::size_t length =
        std::min(kMediaScanChunkSize, end - offset);
    security::SecureBuffer bytes = storage.read(offset, length);
    if (!std::ranges::all_of(bytes.bytes(), [](const std::byte value) {
          return value == kBlankUserDataByte;
        })) {
      return false;
    }
    offset += length;
  }
  return true;
}

void write_verified_zeros(ICardRecordJournalStorage& storage,
                          const std::size_t offset,
                          const std::size_t length) {
  security::SecureBuffer zeros(length);
  try {
    storage.write(offset, zeros.bytes());
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_card_record_io", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    // A vendor-reported failure is indeterminate until readback. Accept it
    // only when every intended zero is nevertheless present.
  }

  if (!range_is_blank(storage, offset, offset + length)) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey zero write did not verify");
  }
}

[[nodiscard]] VikeyCardMediaState inspect_vikey_card_media(
    VikeyCardRecordStorage& storage) {
  const std::size_t capacity = storage.capacity();
  if (capacity < kCardRecordJournalV1StorageSize) {
    throw DiagnosticError(
        make_error_code(CardError::invalid_configuration), DiagnosticStage::sdk_capacity,
        "ViKey user data area is too small for media inspection");
  }

  const CardRecordJournalMediaState journal_state =
      inspect_card_record_journal_media_v1(storage);
  if (journal_state == CardRecordJournalMediaState::product_evidence) {
    return VikeyCardMediaState::product_evidence;
  }
  if (journal_state ==
      CardRecordJournalMediaState::provisioning_interrupted) {
    return range_is_blank(storage, kCardRecordJournalV1StorageSize,
                          capacity)
               ? VikeyCardMediaState::provisioning_interrupted
               : VikeyCardMediaState::conflicting_evidence;
  }

  const bool prefix_blank = range_is_blank(storage, 0, kVikeyBlobBytes);
  const bool remainder_blank =
      range_is_blank(storage, kVikeyBlobBytes, capacity);
  if (prefix_blank && remainder_blank) {
    return VikeyCardMediaState::blank;
  }
  if (!prefix_blank && remainder_blank) {
    return VikeyCardMediaState::legacy_p1_or_foreign_prefix;
  }
  return VikeyCardMediaState::foreign_data;
}

}  // namespace

VikeyCardMediaState inspect_vikey_card_media(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return inspect_vikey_card_media(storage);
}

CardRecordState inspect_vikey_card_record_state_read_only(
    VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return inspect_card_record_journal_logical_state_v1_read_only(storage);
}

void bootstrap_vikey_card_media(
    VikeySession& session,
    const VikeyCardMediaState expected_state) {
  if (expected_state != VikeyCardMediaState::blank &&
      expected_state != VikeyCardMediaState::provisioning_interrupted) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey bootstrap requires an eligible expected media state");
  }

  VikeyCardRecordStorage storage(session);
  if (inspect_vikey_card_media(storage) != expected_state) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey card media changed before bootstrap");
  }

  security::SecureBuffer marker =
      provision_uninitialized_card_record_v1();
  bootstrap_uninitialized_card_record_journal_v1(storage,
                                                  marker.bytes());
  if (!range_is_blank(storage, kCardRecordJournalV1StorageSize,
                      storage.capacity())) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey card media changed during bootstrap");
  }
}

void reset_vikey_development_legacy_media(
    VikeySession& session,
    const VikeyCardMediaState expected_state) {
  if (expected_state !=
      VikeyCardMediaState::legacy_p1_or_foreign_prefix) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey development reset requires the legacy-shaped expected state");
  }

  VikeyCardRecordStorage storage(session);
  if (inspect_vikey_card_media(storage) != expected_state) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey card media changed before development reset");
  }

  const std::size_t capacity = storage.capacity();
  for (std::size_t offset = kVikeyBlobBytes; offset < capacity;) {
    const std::size_t length =
        std::min(kMediaScanChunkSize, capacity - offset);
    write_verified_zeros(storage, offset, length);
    offset += length;
  }

  // Preserve the only legacy-shaped evidence until every other user-data
  // byte has been verified blank. A torn final write is therefore either
  // still eligible for an explicit retry or already a complete blank card.
  write_verified_zeros(storage, 0, kVikeyBlobBytes);
  if (inspect_vikey_card_media(storage) != VikeyCardMediaState::blank) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey development reset did not produce blank media");
  }
}

security::SecureBuffer read_vikey_card_record(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return read_card_record_journal_v1(storage);
}

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record) {
  VikeyCardRecordStorage storage(session);
  commit_card_record_journal_v1(storage, record.bytes());
}

security::SecureBuffer snapshot_vikey_card_record(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return snapshot_card_record_journal_v1(storage);
}
void reset_vikey_card_record(VikeySession& session, const security::SecureBuffer& expected) {
  // Refuse every record reset_card_record_journal_v1 would refuse before
  // touching any byte, so a refused reset never clears the contact list.
  const CardRecordState expected_state = inspect_card_record(expected.bytes());
  if (expected_state != CardRecordState::initialized_v1 &&
      expected_state != CardRecordState::unsupported_version) {
    throw std::system_error(make_error_code(CardError::invalid_operation),
                            "reset requires an initialized or unsupported record");
  }
  VikeyCardRecordStorage storage(session);
  // The contact list (P5.14) is cleared before the journal, and only once the
  // confirmed snapshot still matches. An interrupted clear leaves the card
  // initialized, so the reset can be retried; clearing afterwards could leave
  // an uninitialized card, which reset no longer accepts, still holding
  // contact ciphertext under the old root secret. Zeros, not random bytes, so
  // the range matches what blank-media checks expect. Smaller cards never
  // held a contact list and their bytes beyond 1808 stay untouched.
  if (storage.capacity() >= contacts::kContactStoreV1RegionEnd) {
    const security::SecureBuffer current =
        snapshot_card_record_journal_v1(storage);
    if (!std::ranges::equal(current.bytes(), expected.bytes())) {
      throw std::system_error(make_error_code(CardError::invalid_operation),
                              "confirmed card record changed");
    }
    for (std::size_t offset = contacts::kContactStoreV1RegionOffset;
         offset < contacts::kContactStoreV1RegionEnd;) {
      const std::size_t length = std::min(
          kMediaScanChunkSize, contacts::kContactStoreV1RegionEnd - offset);
      write_verified_zeros(storage, offset, length);
      offset += length;
    }
  }
  reset_card_record_journal_v1(storage, expected.bytes());
}

security::SecureBuffer snapshot_stable_vikey_card_record(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return snapshot_stable_card_record_journal_v1(storage);
}

void write_vikey_spare_card(VikeySession& session,
                            const security::SecureBuffer& record) {
  if (inspect_card_record(record.bytes()) != CardRecordState::initialized_v1)
    throw std::system_error(make_error_code(CardError::invalid_configuration),
                            "spare card requires an initialized v1 record");
  VikeyCardRecordStorage storage(session);
  if (storage.capacity() < kCardRecordJournalV1ReservedSize)
    throw std::system_error(make_error_code(CardError::invalid_configuration),
                            "spare card capacity is insufficient");
  const auto media = inspect_vikey_card_media(storage);
  if (media == VikeyCardMediaState::blank) {
    bootstrap_vikey_card_media(session, media);
  } else if (media != VikeyCardMediaState::product_evidence ||
             inspect_vikey_card_record_state_read_only(session) !=
                 CardRecordState::provisioned_uninitialized) {
    throw std::system_error(make_error_code(CardError::invalid_operation),
                            "spare card target is not empty");
  }
  // Both destination authorization and publication happen in this one
  // hardware-bound session. Never route through an arbitrary update target.
  const auto expected = provision_uninitialized_card_record_v1();
  const auto current = snapshot_vikey_card_record(session);
  if (!std::ranges::equal(current.bytes(), expected.bytes()))
    throw std::system_error(make_error_code(CardError::invalid_operation),
                            "spare card target changed");
  commit_card_record_journal_v1(storage, record.bytes());
  const auto observed = snapshot_vikey_card_record(session);
  if (!std::ranges::equal(observed.bytes(), record.bytes()))
    throw std::system_error(make_error_code(CardError::device_failure),
                            "spare card readback mismatch");
}

namespace {

void require_contact_region_access(VikeyCardRecordStorage& storage) {
  if (storage.capacity() < contacts::kContactStoreV1RegionEnd) {
    throw DiagnosticError(
        make_error_code(CardError::invalid_configuration),
        DiagnosticStage::sdk_capacity,
        "ViKey user data area is too small for the contact list");
  }
  if (inspect_card_record_journal_logical_state_v1_read_only(storage) !=
      CardRecordState::initialized_v1) {
    throw std::system_error(
        make_error_code(CardError::invalid_operation),
        "contact list requires an initialized card record");
  }
}

}  // namespace

security::SecureBuffer read_vikey_contact_region(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  require_contact_region_access(storage);
  return storage.read(contacts::kContactStoreV1RegionOffset,
                      contacts::kContactStoreV1RegionSize);
}

void write_vikey_contact_copy(VikeySession& session,
                              const std::size_t copy_index,
                              const security::SecureBuffer& copy) {
  if (copy_index > 1 || copy.size() != contacts::kContactStoreV1CopySize) {
    throw std::system_error(make_error_code(CardError::invalid_configuration),
                            "invalid contact list copy");
  }
  VikeyCardRecordStorage storage(session);
  require_contact_region_access(storage);
  storage.write(contacts::kContactStoreV1CopyOffsets[copy_index],
                copy.bytes());
}

}  // namespace usb_vault::card::vikey
