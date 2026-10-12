#pragma once
#include "card/vikey/vikey_helper_client.h"
#include "card/vikey/vikey_helper_backend.h"
#include <chrono>

namespace usb_vault::card::vikey {

class IVikeyCardBackupBackend {
 public:
  virtual ~IVikeyCardBackupBackend() = default;
  virtual VikeyCardMediaProbe probe() = 0;
  virtual security::SecureBuffer snapshot(std::uint32_t identity) = 0;
  virtual void write(std::uint32_t identity, const security::SecureBuffer& record) = 0;
};

class VikeyHelperCardBackupBackend final : public IVikeyCardBackupBackend {
 public:
  VikeyHelperCardBackupBackend(VikeyHelperBackendConfig config,
                              IVikeyCredentialProvider& credentials);
  VikeyCardMediaProbe probe() override;
  security::SecureBuffer snapshot(std::uint32_t identity) override;
  void write(std::uint32_t identity, const security::SecureBuffer& record) override;
 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credentials_;
};

// Owns only encrypted record bytes across an intentional physical exchange.
// No application password, decrypted root, file, or persistent card group.
class VikeyCardBackup final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit VikeyCardBackup(IVikeyCardBackupBackend& backend) : backend_(backend) {}
  void prepare(Clock::time_point now = Clock::now());
  void complete(Clock::time_point now = Clock::now());
  void cancel() noexcept;
  bool pending(Clock::time_point now = Clock::now()) noexcept;
 private:
  IVikeyCardBackupBackend& backend_;
  security::SecureBuffer record_;
  std::uint32_t source_{};
  Clock::time_point deadline_{};
};
}  // namespace usb_vault::card::vikey
