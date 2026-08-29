#pragma once

#include "usb_vault/card/card_store.h"
#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::card {

// Vendor-neutral boundary for one insertion context that an adapter has
// already classified as eligible bare media. Implementations must bind both
// operations to the same physical identity. Ensuring the provisioning record
// is idempotent only after a verified success; an uncertain failure must reach
// the caller and be resolved by a fresh insertion probe, never an automatic
// retry or silent rebind.
class ICardInitializationTarget {
 public:
  virtual ~ICardInitializationTarget() noexcept = default;

  ICardInitializationTarget(const ICardInitializationTarget&) = delete;
  ICardInitializationTarget& operator=(const ICardInitializationTarget&) =
      delete;
  ICardInitializationTarget(ICardInitializationTarget&&) = delete;
  ICardInitializationTarget& operator=(ICardInitializationTarget&&) = delete;

  virtual void ensure_provisioning_record() = 0;
  [[nodiscard]] virtual ICardStore& card_store() noexcept = 0;

 protected:
  ICardInitializationTarget() noexcept = default;
};

// Executes only the product-level write transaction for an adapter-approved
// new card. The password is consumed and must already have been confirmed and
// accepted by the UI password policy. This layer deliberately does not know a
// vendor, hardware identity representation, media classification, UI state,
// volume shape, or password role beyond "daily login password".
class CardInitializationService final {
 public:
  [[nodiscard]] static security::SecureBuffer initialize_and_authenticate(
      ICardInitializationTarget& target,
      security::SecureBuffer validated_daily_password_utf8);
};

}  // namespace usb_vault::card
