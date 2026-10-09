#pragma once

#include "card/card_record_journal_v2.h"

namespace usb_vault::card {
// Destructive reset is authorized by native confirmation, not password knowledge.
// This opaque ciphertext-only plan also permits an exact interrupted-write retry.
class PreparedCardJournalV2Reset final {
 public:
  PreparedCardJournalV2Reset(PreparedCardJournalV2Reset&&) noexcept = default;
  PreparedCardJournalV2Reset& operator=(PreparedCardJournalV2Reset&&) noexcept = default;
  PreparedCardJournalV2Reset(const PreparedCardJournalV2Reset&) = delete;
  PreparedCardJournalV2Reset& operator=(const PreparedCardJournalV2Reset&) = delete;
 private:
  PreparedCardJournalV2Reset() = default;
  friend PreparedCardJournalV2Reset prepare_card_journal_v2_reset(
      ICardRecordJournalStorage&, std::span<const std::byte>);
  friend void reset_card_journal_v2(ICardRecordJournalStorage&, const PreparedCardJournalV2Reset&);
  security::SecureBuffer source_;
  security::SecureBuffer target_;
};
[[nodiscard]] PreparedCardJournalV2Reset prepare_card_journal_v2_reset(
    ICardRecordJournalStorage& storage, std::span<const std::byte> expected_reserved);
void reset_card_journal_v2(ICardRecordJournalStorage& storage,
    const PreparedCardJournalV2Reset& prepared);
} // namespace usb_vault::card
