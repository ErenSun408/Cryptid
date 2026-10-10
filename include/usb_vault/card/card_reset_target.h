#pragma once
#include "usb_vault/security/secure_buffer.h"
namespace usb_vault::card {
// Bound to one adapter-authenticated physical card. Snapshot must never write.
// Conditional reset compares expected bytes and writes in the same SDK session.
class ICardResetTarget {
 public:
  virtual ~ICardResetTarget() noexcept = default;
  // V2 targets prepare and bind a complete reserved-region image internally.
  [[nodiscard]] virtual bool uses_reserved_snapshot() const noexcept { return false; }
  [[nodiscard]] virtual security::SecureBuffer snapshot() = 0;
  virtual void reset(const security::SecureBuffer& expected) = 0;
};
}  // namespace usb_vault::card
