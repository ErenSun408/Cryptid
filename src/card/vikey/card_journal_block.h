#pragma once
#include "card/card_record_journal_v2.h"
#include "usb_vault/card/card_record_v2.h"

namespace usb_vault::card::vikey {
// Fixed physical blocks only; this transport predicate does not authenticate
// records or authorize a transaction. The application must do that first.
[[nodiscard]] constexpr bool valid_card_journal_block(
    const std::size_t offset, const std::size_t length) noexcept {
  return (length == kCardRecordJournalV1DescriptorSize &&
             (offset == kCardRecordJournalV1Descriptor0Offset ||
              offset == kCardRecordJournalV1Descriptor1Offset ||
              offset == kCardRecordJournalV2Descriptor0Offset ||
              offset == kCardRecordJournalV2Descriptor1Offset)) ||
         (length == kCardRecordV1Size &&
             (offset == kCardRecordJournalV1Slot0Offset || offset == kCardRecordJournalV1Slot1Offset)) ||
         (length == kCardRecordV2Size &&
             (offset == kCardRecordJournalV2Slot0Offset || offset == kCardRecordJournalV2Slot1Offset));
}
} // namespace usb_vault::card::vikey
