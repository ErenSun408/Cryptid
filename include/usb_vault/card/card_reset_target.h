#pragma once
#include "usb_vault/security/secure_buffer.h"
namespace usb_vault::card {
// Bound to one adapter-authenticated physical card. Snapshot must never write.
// Conditional reset compares expected bytes and writes in the same SDK session.
class ICardResetTarget {
 public:
  virtual ~ICardResetTarget() noexcept = default;
  [[nodiscard]] virtual security::SecureBuffer snapshot() = 0;
  virtual void reset(const security::SecureBuffer& expected) = 0;
};
}  // namespace usb_vault::card
