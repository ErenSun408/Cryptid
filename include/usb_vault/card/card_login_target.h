#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/card/card_product_record_state.h"
#include <stdexcept>

namespace usb_vault::card {

// Vendor-neutral, short-lived access target for one adapter-verified
// initialized insertion. It exposes no arbitrary ICardStore write capability.
// Opt-in v2 targets additionally finish narrowly scoped, authenticated journal
// recovery before handoff. Implementations bind operations to the identity captured
// by the corresponding read-only entry context.
class ICardLoginTarget {
 public:
  virtual ~ICardLoginTarget() noexcept = default;

  ICardLoginTarget(const ICardLoginTarget&) = delete;
  ICardLoginTarget& operator=(const ICardLoginTarget&) = delete;
  ICardLoginTarget(ICardLoginTarget&&) = delete;
  ICardLoginTarget& operator=(ICardLoginTarget&&) = delete;

  [[nodiscard]] virtual security::SecureBuffer read_record() = 0;
  [[nodiscard]] virtual CardProductRecordState record_format() const noexcept {
    return CardProductRecordState::initialized_v1;
  }
  // Required for v2, after password authentication and the logical recheck,
  // immediately before handoff. The root stays in the application; verify the
  // exact record and its anchor before any normalization. The default refuses
  // v2 handoff, so declaring v2 without implementing this gate fails closed.
  virtual void complete_authenticated_read(std::span<const std::byte>,
      const security::SecureBuffer&) {
    throw std::logic_error("authenticated journal verification unsupported");
  }

 protected:
  ICardLoginTarget() noexcept = default;
};

}  // namespace usb_vault::card
