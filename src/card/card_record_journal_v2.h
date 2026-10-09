#pragma once

#include "card/card_record_journal_v1.h"
#include "card/card_record_journal_v2_anchor.h"

#include <array>
#include <utility>

namespace usb_vault::card {

inline constexpr std::size_t kCardRecordJournalV2DescriptorSize = 152;
inline constexpr std::size_t kCardRecordJournalV2Descriptor0Offset = 736;
inline constexpr std::size_t kCardRecordJournalV2Descriptor1Offset = 888;
inline constexpr std::size_t kCardRecordJournalV2Slot0Offset = 1040;
inline constexpr std::size_t kCardRecordJournalV2Slot1Offset = 1424;
inline constexpr std::size_t kCardRecordJournalV2StorageSize = 1808;

class PreparedCardJournalV2Initialization;

class PreparedCardJournalV2Migration final {
 public:
  PreparedCardJournalV2Migration(PreparedCardJournalV2Migration&&) noexcept = default;
  PreparedCardJournalV2Migration& operator=(PreparedCardJournalV2Migration&&) noexcept = default;
  PreparedCardJournalV2Migration(const PreparedCardJournalV2Migration&) = delete;
  PreparedCardJournalV2Migration& operator=(const PreparedCardJournalV2Migration&) = delete;
  [[nodiscard]] std::span<const std::byte> replacement_record() const noexcept {
    return replacement_.bytes();
  }

 private:
  PreparedCardJournalV2Migration() = default;
  friend PreparedCardJournalV2Migration prepare_card_journal_v2_migration(
      ICardRecordJournalStorage&, std::span<const std::byte>, security::SecureBuffer);
  friend void commit_card_journal_v2_migration(
      ICardRecordJournalStorage&, const PreparedCardJournalV2Migration&);
  friend PreparedCardJournalV2Initialization prepare_card_journal_v2_initialization(
      ICardRecordJournalStorage&, std::span<const std::byte>, security::SecureBuffer);
  friend security::SecureBuffer commit_card_journal_v2_initialization(
      ICardRecordJournalStorage&, const PreparedCardJournalV2Initialization&);
  security::SecureBuffer root_;
  security::SecureBuffer expected_;
  security::SecureBuffer replacement_;
  security::SecureBuffer anchor_;
  std::array<std::byte, 16> source_commit_{};
  std::array<std::byte, 16> intent_{};
  std::array<std::byte, 16> instance_{};
  std::array<std::byte, 16> initial_commit_{};
  bool initializing_{};
};

class PreparedCardJournalV2Initialization final {
 public:
  PreparedCardJournalV2Initialization(PreparedCardJournalV2Initialization&&) noexcept = default;
  PreparedCardJournalV2Initialization& operator=(PreparedCardJournalV2Initialization&&) noexcept = default;
  PreparedCardJournalV2Initialization(const PreparedCardJournalV2Initialization&) = delete;
  PreparedCardJournalV2Initialization& operator=(const PreparedCardJournalV2Initialization&) = delete;
 private:
  friend PreparedCardJournalV2Initialization prepare_card_journal_v2_initialization(
      ICardRecordJournalStorage&, std::span<const std::byte>, security::SecureBuffer);
  friend security::SecureBuffer commit_card_journal_v2_initialization(
      ICardRecordJournalStorage&, const PreparedCardJournalV2Initialization&);
  explicit PreparedCardJournalV2Initialization(PreparedCardJournalV2Migration&& plan)
      : publication_(std::move(plan)) {}
  PreparedCardJournalV2Migration publication_;
};

// A healthy provisioning journal is required; bare-media classification and
// bootstrap remain the hardware adapter's responsibility. Preparation is read-only.
// Retain the opaque plan across uncertain writes. A lost plan can be rebuilt only
// after its complete staged v2 record authenticates the root-bound reservation.
[[nodiscard]] PreparedCardJournalV2Initialization prepare_card_journal_v2_initialization(
    ICardRecordJournalStorage& storage, std::span<const std::byte> expected_marker,
    security::SecureBuffer password_utf8);
[[nodiscard]] security::SecureBuffer commit_card_journal_v2_initialization(
    ICardRecordJournalStorage& storage, const PreparedCardJournalV2Initialization& prepared);

// Adapter-private storage operations: the caller must hold its card operation
// lease and stable hardware identity for the whole transaction. These routines
// do not load vendor code and never obtain or emit credentials themselves.
// Root-bearing calls stay in the authenticated application process; storage
// adapters must not serialize the root into helper IPC or vendor calls.
[[nodiscard]] PreparedCardJournalV2Migration prepare_card_journal_v2_migration(
    ICardRecordJournalStorage& storage, std::span<const std::byte> expected_v1,
    security::SecureBuffer existing_password_utf8);
void commit_card_journal_v2_migration(ICardRecordJournalStorage& storage,
    const PreparedCardJournalV2Migration& prepared);

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
