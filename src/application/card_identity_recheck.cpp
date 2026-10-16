#include "usb_vault/application/card_identity_recheck.h"

namespace usb_vault::application {

CardIdentityVerdict CardIdentityRecheckPolicy::observe(
    const CardIdentityRecheck result) noexcept {
  switch (result) {
    case CardIdentityRecheck::same_card:
      unreadable_pending_ = false;
      return CardIdentityVerdict::present;
    case CardIdentityRecheck::busy:
      return CardIdentityVerdict::skipped;
    case CardIdentityRecheck::unreadable:
      if (!unreadable_pending_) {
        unreadable_pending_ = true;
        return CardIdentityVerdict::retry;
      }
      break;
    case CardIdentityRecheck::different_card:
    case CardIdentityRecheck::credentials_rejected:
      break;
  }
  unreadable_pending_ = false;
  return CardIdentityVerdict::lost;
}

void CardIdentityRecheckPolicy::reset() noexcept { unreadable_pending_ = false; }

}  // namespace usb_vault::application
