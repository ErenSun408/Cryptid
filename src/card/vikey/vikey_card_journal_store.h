#pragma once

#include "card/vikey/vikey_helper_backend.h"
#include "card/card_record_journal_v2.h"
#include "card/card_record_journal_v2_reset.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/card/card_product_record_state.h"
#include "usb_vault/card/card_login_target.h"

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

// Opaque initialization material retained only for an uncertain write retry.
class PreparedVikeyCardInitialization final {
 public:
  PreparedVikeyCardInitialization(PreparedVikeyCardInitialization&&) noexcept = default;
  PreparedVikeyCardInitialization& operator=(PreparedVikeyCardInitialization&&) noexcept = default;
  PreparedVikeyCardInitialization(const PreparedVikeyCardInitialization&) = delete;
  PreparedVikeyCardInitialization& operator=(const PreparedVikeyCardInitialization&) = delete;
 private:
  friend class VikeyCardJournalStore;
  PreparedVikeyCardInitialization(PreparedCardJournalV2Initialization plan,
      std::shared_ptr<const std::byte> owner)
      : plan_(std::move(plan)), owner_(std::move(owner)) {}
  PreparedCardJournalV2Initialization plan_;
  std::shared_ptr<const std::byte> owner_;
};

// Ciphertext-only reset material, bound to one store instance and source image.
class PreparedVikeyCardReset final {
 public:
  PreparedVikeyCardReset(PreparedVikeyCardReset&&) noexcept = default;
  PreparedVikeyCardReset& operator=(PreparedVikeyCardReset&&) noexcept = default;
  PreparedVikeyCardReset(const PreparedVikeyCardReset&) = delete;
  PreparedVikeyCardReset& operator=(const PreparedVikeyCardReset&) = delete;
 private:
  friend class VikeyCardJournalStore;
  PreparedVikeyCardReset(PreparedCardJournalV2Reset plan, std::shared_ptr<const std::byte> owner)
      : plan_(std::move(plan)), owner_(std::move(owner)) {}
  PreparedCardJournalV2Reset plan_;
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
  // Strictly read-only, unauthenticated admission evidence for the new entry
  // contract. No slot/cursor/root information is returned to the caller.
  [[nodiscard]] CardProductRecordState inspect_entry();
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
  [[nodiscard]] security::SecureBuffer snapshot_provisioning();
  [[nodiscard]] PreparedVikeyCardInitialization prepare_initialization(
      std::span<const std::byte> expected_marker, security::ConfirmedPassword password);
  // Retain the locked plan across uncertain writes so retry keeps the same root.
  // Lifecycle/confirmation ownership remains with the application caller.
  [[nodiscard]] security::SecureBuffer initialize(
      const PreparedVikeyCardInitialization& prepared);
  [[nodiscard]] security::SecureBuffer snapshot_for_reset();
  [[nodiscard]] PreparedVikeyCardReset prepare_reset(std::span<const std::byte> expected_reserved);
  // Native destructive confirmation is required before this private entry.
  // Low-level callers may retain the plan across uncertain writes. Product
  // targets discard it after one attempt and require a freshly confirmed image.
  void reset(const PreparedVikeyCardReset& prepared);
 private:
  IVikeyCardJournalTransport& transport_;
  const std::shared_ptr<const std::byte> owner_{std::make_shared<const std::byte>()};
};
// Side-effect-free construction over one hardware-bound transport. Production
// source composition must keep the transport alive through target destruction.
class VikeyCardJournalLoginTarget final : public ICardLoginTarget {
 public:
  explicit VikeyCardJournalLoginTarget(IVikeyCardJournalTransport& transport) : store_(transport) {}
  [[nodiscard]] security::SecureBuffer read_record() override;
  [[nodiscard]] CardProductRecordState record_format() const noexcept override {
    return CardProductRecordState::initialized_v2;
  }
  void complete_authenticated_read(std::span<const std::byte> expected,
      const security::SecureBuffer& root) override;
 private:
  VikeyCardJournalStore store_;
};
} // namespace usb_vault::card::vikey
