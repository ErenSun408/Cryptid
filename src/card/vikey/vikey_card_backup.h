#pragma once
#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_backend.h"
#include <chrono>
#include <optional>

namespace usb_vault::card::vikey {

class IVikeyCardBackupBackend {
 public:
  virtual ~IVikeyCardBackupBackend() = default;
  virtual VikeyCardMediaProbe probe() = 0;
  virtual security::SecureBuffer snapshot(std::uint32_t identity) = 0;
  // Whole contact region ciphertext (P5.14); nothing when the card's user area
  // cannot hold a contact list.
  virtual std::optional<security::SecureBuffer> contact_region(std::uint32_t identity) = 0;
  // contact_region, when given, is copied in the same restricted session
  // before the record is committed.
  virtual void write(std::uint32_t identity, const security::SecureBuffer& record,
                     const security::SecureBuffer* contact_region) = 0;
};

class VikeyHelperCardBackupBackend final : public IVikeyCardBackupBackend {
 public:
  VikeyHelperCardBackupBackend(VikeyHelperBackendConfig config,
                              IVikeyCredentialProvider& credentials);
  VikeyCardMediaProbe probe() override;
  security::SecureBuffer snapshot(std::uint32_t identity) override;
  std::optional<security::SecureBuffer> contact_region(std::uint32_t identity) override;
  void write(std::uint32_t identity, const security::SecureBuffer& record,
             const security::SecureBuffer* contact_region) override;
 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credentials_;
};

// Owns only encrypted record and contact-region bytes across an intentional
// physical exchange. No application password, decrypted root, file, or
// persistent card group.
class VikeyCardBackup final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit VikeyCardBackup(IVikeyCardBackupBackend& backend) : backend_(backend) {}
  void prepare(Clock::time_point now = Clock::now());
  void complete(Clock::time_point now = Clock::now(), std::uint32_t expected_target = 0);
  bool target_eligible(const VikeyCardMediaProbe& target, Clock::time_point now = Clock::now()) noexcept;
  bool is_source(const VikeyCardMediaProbe& target) const noexcept {
    return source_ != 0 && target.hardware_id == source_;
  }
  void cancel() noexcept;
  bool pending(Clock::time_point now = Clock::now()) noexcept;
  unsigned int remaining_seconds(Clock::time_point now = Clock::now()) noexcept;
 private:
  IVikeyCardBackupBackend& backend_;
  security::SecureBuffer record_;
  std::optional<security::SecureBuffer> contacts_;
  std::uint32_t source_{};
  Clock::time_point deadline_{};
};
}  // namespace usb_vault::card::vikey
