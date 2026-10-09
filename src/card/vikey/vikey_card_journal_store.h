#pragma once

#include "card/vikey/vikey_helper_backend.h"
#include "card/card_record_journal_v2.h"

#include <memory>
#include <utility>

namespace usb_vault::card::vikey {

// Ciphertext-only seam. Every implementation binds each call to the same
// nonzero hardware identity; no application password or root is accepted.
class IVikeyCardJournalTransport {
 public:
  virtual ~IVikeyCardJournalTransport() noexcept = default;
  [[nodiscard]] virtual security::SecureBuffer snapshot_reserved() = 0;
  virtual void write_block(const security::SecureBuffer& expected_snapshot,
      std::size_t offset, const security::SecureBuffer& replacement) = 0;
};

// Current network-enabled development boundary, with fresh locked vendor
// credentials for every one-shot call and all existing helper checks.
class VikeyHelperCardJournalTransport final : public IVikeyCardJournalTransport {
 public:
  VikeyHelperCardJournalTransport(VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credentials, std::uint32_t expected_hardware_id);
  [[nodiscard]] security::SecureBuffer snapshot_reserved() override;
  void write_block(const security::SecureBuffer& expected_snapshot,
      std::size_t offset, const security::SecureBuffer& replacement) override;
 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credentials_;
  std::uint32_t hardware_id_{};
};

// Opaque, move-only upgrade material, with a root in locked process memory.
class PreparedVikeyCardUpgrade final {
 public:
  PreparedVikeyCardUpgrade(PreparedVikeyCardUpgrade&&) noexcept = default;
  PreparedVikeyCardUpgrade& operator=(PreparedVikeyCardUpgrade&&) noexcept = default;
  PreparedVikeyCardUpgrade(const PreparedVikeyCardUpgrade&) = delete;
  PreparedVikeyCardUpgrade& operator=(const PreparedVikeyCardUpgrade&) = delete;
 private:
  friend class VikeyCardJournalStore;
  PreparedVikeyCardUpgrade(PreparedCardJournalV2Migration plan,
      std::shared_ptr<const std::byte> owner)
      : plan_(std::move(plan)), owner_(std::move(owner)) {}
  PreparedCardJournalV2Migration plan_;
  // Keeps the instance identity alive even after its store is destroyed.
  std::shared_ptr<const std::byte> owner_;
};

// Application-process adapter. Each method holds a complete operation lease,
// including verification and cleanup. Authenticate snapshot() via the v2 logical
// API first; recover/commit verify the root and anchor before any write. Legacy
// ICardStore and page routing are unchanged; the root never enters transport.
class VikeyCardJournalStore final {
 public:
  explicit VikeyCardJournalStore(IVikeyCardJournalTransport& transport)
      : transport_(transport) {}
  VikeyCardJournalStore(const VikeyCardJournalStore&) = delete;
  VikeyCardJournalStore& operator=(const VikeyCardJournalStore&) = delete;
  [[nodiscard]] security::SecureBuffer snapshot();
  void recover(std::span<const std::byte> expected_record,
      const security::SecureBuffer& authenticated_root);
  void commit(std::span<const std::byte> expected_record,
      std::span<const std::byte> replacement_record,
      const security::SecureBuffer& authenticated_root);
  // Only initialized v1 records; no normalization or write during preparation.
  [[nodiscard]] security::SecureBuffer snapshot_legacy();
  [[nodiscard]] PreparedVikeyCardUpgrade prepare_upgrade(
      std::span<const std::byte> expected_v1, security::SecureBuffer password_utf8);
  // Caller must obtain native one-shot confirmation before invoking this private
  // adapter entry. Every attempt consumes the plan; failures require fresh
  // authentication/preparation, or v2 authentication/recovery after publication.
  void commit_upgrade(PreparedVikeyCardUpgrade&& prepared);
 private:
  IVikeyCardJournalTransport& transport_;
  const std::shared_ptr<const std::byte> owner_{std::make_shared<const std::byte>()};
};
} // namespace usb_vault::card::vikey
