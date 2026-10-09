#pragma once

#include "usb_vault/card/card_store.h"
#include "usb_vault/security/confirmed_password.h"

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

  virtual void ensure_provisioning_record();
  [[nodiscard]] virtual ICardStore& card_store();
  // The default preserves the legacy transaction. Journal targets override
  // this complete operation and never expose an arbitrary blob writer.
  [[nodiscard]] virtual security::SecureBuffer initialize_confirmed(
      security::ConfirmedPassword password);

 protected:
  ICardInitializationTarget() noexcept = default;
};

// Executes only the product-level write transaction for an adapter-approved
// new card. The unforgeable password proof is consumed; raw unchecked password
// buffers cannot call this API. This layer deliberately does not know a
// vendor, hardware identity representation, media classification, UI state,
// volume shape, or password role beyond "daily login password".
class CardInitializationService final {
 public:
  [[nodiscard]] static security::SecureBuffer initialize_and_authenticate(
      ICardInitializationTarget& target,
      security::ConfirmedPassword confirmed_daily_password);
};

}  // namespace usb_vault::card
