#pragma once

#include "card/vikey/vikey_helper_backend.h"

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

// Application-process adapter. Each method holds a complete operation lease,
// including verification and cleanup. Authenticate snapshot() via the v2 logical
// API first; recover/commit verify the root and anchor before any write. Legacy
// ICardStore and page routing are unchanged; the root never enters transport.
class VikeyCardJournalStore final {
 public:
  explicit VikeyCardJournalStore(IVikeyCardJournalTransport& transport) noexcept
      : transport_(transport) {}
  [[nodiscard]] security::SecureBuffer snapshot();
  void recover(std::span<const std::byte> expected_record,
      const security::SecureBuffer& authenticated_root);
  void commit(std::span<const std::byte> expected_record,
      std::span<const std::byte> replacement_record,
      const security::SecureBuffer& authenticated_root);
 private:
  IVikeyCardJournalTransport& transport_;
};
} // namespace usb_vault::card::vikey
