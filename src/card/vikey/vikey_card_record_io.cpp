#include "card/vikey/vikey_card_record_io.h"

#include "card/card_record_journal_v1.h"
#include "card/vikey/vikey_session.h"
#include "usb_vault/card/card_error.h"

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

}  // namespace

VikeyCardMediaState inspect_vikey_card_media(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  const std::size_t capacity = storage.capacity();
  if (capacity < kCardRecordJournalV1StorageSize) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
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

  const bool prefix_blank =
      range_is_blank(storage, 0, kVikeyBlobBytes);
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

security::SecureBuffer read_vikey_card_record(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return read_card_record_journal_v1(storage);
}

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record) {
  VikeyCardRecordStorage storage(session);
  commit_card_record_journal_v1(storage, record.bytes());
}

}  // namespace usb_vault::card::vikey
