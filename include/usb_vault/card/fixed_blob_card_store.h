#pragma once

#include "usb_vault/card/card_store.h"

namespace usb_vault::card {

// P1.5-only card substitute. It generates the deterministic test blob directly
// in locked memory on each read and retains no secret or mutable card state.
class FixedBlobCardStore final : public ICardStore {
 public:
  [[nodiscard]] security::SecureBuffer get_blob() override;
  void put_blob(const security::SecureBuffer& blob) override;
  [[nodiscard]] CardCapabilities capabilities() const noexcept override;
};

}  // namespace usb_vault::card
