#pragma once

#include "card/card_record_journal_v1.h"
#include "card/card_record_journal_v2_anchor.h"

#include <array>

namespace usb_vault::card {

inline constexpr std::size_t kCardRecordJournalV2DescriptorSize = 152;
inline constexpr std::size_t kCardRecordJournalV2Descriptor0Offset = 736;
inline constexpr std::size_t kCardRecordJournalV2Descriptor1Offset = 888;
inline constexpr std::size_t kCardRecordJournalV2Slot0Offset = 1040;
inline constexpr std::size_t kCardRecordJournalV2Slot1Offset = 1424;
inline constexpr std::size_t kCardRecordJournalV2StorageSize = 1808;

// Adapter-private storage operations: the caller must hold its card operation
// lease and stable hardware identity for the whole transaction. These routines
// do not load vendor code and never obtain or emit credentials themselves.
// Root-bearing calls stay in the authenticated application process; storage
// adapters must not serialize the root into helper IPC or vendor calls.
[[nodiscard]] security::SecureBuffer snapshot_card_record_journal_v2(
    ICardRecordJournalStorage& storage);
// Verify the immutable anchor after the application has authenticated the
// selected logical record. The journal's public checksums are not MACs.
void verify_card_record_journal_v2_anchor(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected_record,
    const security::SecureBuffer& authenticated_root);
// The caller authenticates the logical record first. These writes additionally
// verify its root-bound anchor and ring metadata before normalization or commit.
// A retry of the exact completed replacement is idempotent;
// all other stale snapshots fail before any writes.
void recover_card_record_journal_v2(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected_record,
    const security::SecureBuffer& authenticated_root);
void commit_card_record_journal_v2(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected_record,
    std::span<const std::byte> replacement_record,
    const security::SecureBuffer& authenticated_root);

} // namespace usb_vault::card
