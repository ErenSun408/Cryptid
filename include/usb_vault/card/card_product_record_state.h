#pragma once

#include "usb_vault/card/card_record_v1.h"

namespace usb_vault::card {
// Application-only format evidence. This does not change the v1 inspector's
// unsupported-version boundary and is never evidence of authentication.
enum class CardProductRecordState {
  provisioned_uninitialized,
  initialized_v1,
  initialized_v2,
  legacy_p1_blob,
  unsupported_version,
  malformed,
};
[[nodiscard]] inline CardProductRecordState product_state_from_legacy(
    const CardRecordState state) noexcept {
  switch (state) {
    case CardRecordState::provisioned_uninitialized: return CardProductRecordState::provisioned_uninitialized;
    case CardRecordState::initialized_v1: return CardProductRecordState::initialized_v1;
    case CardRecordState::legacy_p1_blob: return CardProductRecordState::legacy_p1_blob;
    case CardRecordState::unsupported_version: return CardProductRecordState::unsupported_version;
    case CardRecordState::malformed: return CardProductRecordState::malformed;
  }
  return static_cast<CardProductRecordState>(-1);
}
} // namespace usb_vault::card
