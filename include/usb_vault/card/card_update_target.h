#pragma once

#include "usb_vault/card/card_store.h"

namespace usb_vault::card {

// Vendor-neutral, short-lived read/write target for one adapter-verified
// initialized insertion.
//
// This is deliberately separate from ICardLoginTarget, which exposes no write
// capability at all: logging in must never be able to modify the card, while
// replacing both record envelopes must. Implementations bind every operation
// to the physical identity captured by the corresponding read-only entry
// context, exactly as the initialization target does.
class ICardUpdateTarget {
 public:
  virtual ~ICardUpdateTarget() noexcept = default;

  ICardUpdateTarget(const ICardUpdateTarget&) = delete;
  ICardUpdateTarget& operator=(const ICardUpdateTarget&) = delete;
  ICardUpdateTarget(ICardUpdateTarget&&) = delete;
  ICardUpdateTarget& operator=(ICardUpdateTarget&&) = delete;

  [[nodiscard]] virtual ICardStore& card_store() noexcept = 0;

 protected:
  ICardUpdateTarget() noexcept = default;
};

}  // namespace usb_vault::card
