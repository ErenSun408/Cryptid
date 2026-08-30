#pragma once

#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::card {

// Vendor-neutral, short-lived access target for one adapter-verified
// initialized insertion. It intentionally exposes no ICardStore write
// capability. Implementations bind the read to the physical identity captured
// by the corresponding read-only entry context.
class ICardLoginTarget {
 public:
  virtual ~ICardLoginTarget() noexcept = default;

  ICardLoginTarget(const ICardLoginTarget&) = delete;
  ICardLoginTarget& operator=(const ICardLoginTarget&) = delete;
  ICardLoginTarget(ICardLoginTarget&&) = delete;
  ICardLoginTarget& operator=(ICardLoginTarget&&) = delete;

  [[nodiscard]] virtual security::SecureBuffer read_record() = 0;

 protected:
  ICardLoginTarget() noexcept = default;
};

}  // namespace usb_vault::card
