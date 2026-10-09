#pragma once

#include "card/card_record_journal_v1.h"

#include <array>

namespace usb_vault::card::detail {

struct LegacyCardJournalSnapshot final {
  security::SecureBuffer record;
  std::array<std::byte, 16> commit_id{};
};

[[nodiscard]] LegacyCardJournalSnapshot snapshot_legacy_card_journal(
    ICardRecordJournalStorage& storage);
// Narrow migration transition: only an unchanged initialized record (intent)
// or a strictly framed v2 anchor may replace the expected legacy record.
void commit_legacy_card_journal_transition(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected_record,
    const std::array<std::byte, 16>& expected_commit,
    std::span<const std::byte> replacement,
    const std::array<std::byte, 16>& new_commit);

} // namespace usb_vault::card::detail
