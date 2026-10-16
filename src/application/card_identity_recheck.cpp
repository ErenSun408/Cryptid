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

CardIdentityRecheckSchedule::CardIdentityRecheckSchedule(
    const Clock::duration interval, const Clock::duration retry) noexcept
    : interval_(interval), retry_(retry) {}

bool CardIdentityRecheckSchedule::start_due(const Clock::time_point now,
                                            const bool eligible,
                                            const std::uint64_t card_epoch) noexcept {
  if (in_flight_) return false;
  if (!eligible) {
    policy_.reset();
    next_.reset();
    return false;
  }
  if (!next_) {
    next_ = now + interval_;
    return false;
  }
  if (now < *next_) return false;
  in_flight_ = true;
  started_epoch_ = card_epoch;
  return true;
}

CardIdentityVerdict CardIdentityRecheckSchedule::finish(
    const CardIdentityRecheck result, const Clock::time_point now,
    const bool eligible, const std::uint64_t card_epoch) noexcept {
  in_flight_ = false;
  if (!eligible || card_epoch != started_epoch_) {
    policy_.reset();
    next_.reset();
    return CardIdentityVerdict::skipped;
  }
  const CardIdentityVerdict verdict = policy_.observe(result);
  switch (verdict) {
    case CardIdentityVerdict::lost:
      policy_.reset();
      next_.reset();
      break;
    case CardIdentityVerdict::retry:
      next_ = now + retry_;
      break;
    case CardIdentityVerdict::present:
    case CardIdentityVerdict::skipped:
      next_ = now + interval_;
      break;
  }
  return verdict;
}

void CardIdentityRecheckSchedule::abandon(const Clock::time_point now) noexcept {
  in_flight_ = false;
  next_ = now + interval_;
}

}  // namespace usb_vault::application
