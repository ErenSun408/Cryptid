#include "card/vikey/card_recheck_coordinator.h"
#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/application/card_identity_recheck.h"
#include "usb_vault/card/card_error.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace {

using usb_vault::application::CardIdentityRecheck;
using usb_vault::application::CardIdentityRecheckPolicy;
using usb_vault::application::CardIdentityRecheckSchedule;
using usb_vault::application::CardIdentityVerdict;
using usb_vault::card::CardError;
using usb_vault::card::vikey::CardForegroundOperation;
using usb_vault::card::vikey::CardRecheckCoordinator;
using usb_vault::card::vikey::classify_identity_recheck_failure;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

void policy_retries_one_unreadable_then_counts_removal() {
  CardIdentityRecheckPolicy policy;
  require(policy.observe(CardIdentityRecheck::same_card) == CardIdentityVerdict::present,
          "same card is present");
  require(policy.observe(CardIdentityRecheck::unreadable) == CardIdentityVerdict::retry,
          "first unreadable retries");
  require(policy.observe(CardIdentityRecheck::busy) == CardIdentityVerdict::skipped,
          "busy is skipped");
  require(policy.observe(CardIdentityRecheck::unreadable) == CardIdentityVerdict::lost,
          "busy does not clear a pending unreadable");
  require(policy.observe(CardIdentityRecheck::unreadable) == CardIdentityVerdict::retry,
          "a verdict starts a new count");
  require(policy.observe(CardIdentityRecheck::same_card) == CardIdentityVerdict::present,
          "same card after a retry is present");
  require(policy.observe(CardIdentityRecheck::unreadable) == CardIdentityVerdict::retry,
          "same card clears the pending unreadable");
  policy.reset();
  require(policy.observe(CardIdentityRecheck::unreadable) == CardIdentityVerdict::retry,
          "reset clears the pending unreadable");
}

void policy_loses_at_once_on_other_card_or_refused_credentials() {
  CardIdentityRecheckPolicy policy;
  require(policy.observe(CardIdentityRecheck::different_card) == CardIdentityVerdict::lost,
          "different card is lost");
  require(policy.observe(CardIdentityRecheck::credentials_rejected) == CardIdentityVerdict::lost,
          "refused credentials are never retried");
  require(policy.observe(static_cast<CardIdentityRecheck>(200)) == CardIdentityVerdict::lost,
          "an unknown result fails closed");
}

void schedule_paces_retries_and_discards_stale_results() {
  using Clock = CardIdentityRecheckSchedule::Clock;
  using std::chrono::seconds;
  CardIdentityRecheckSchedule schedule(seconds(30), seconds(3));
  const Clock::time_point t0{};
  require(!schedule.start_due(t0, true, 1), "first eligible tick only arms the interval");
  require(!schedule.start_due(t0 + seconds(29), true, 1), "not due before the interval");
  require(schedule.start_due(t0 + seconds(30), true, 1), "due after the interval");
  require(!schedule.start_due(t0 + seconds(60), true, 1), "one recheck in flight at a time");
  require(schedule.finish(CardIdentityRecheck::unreadable, t0 + seconds(31), true, 1) ==
              CardIdentityVerdict::retry, "first unreadable retries");
  require(!schedule.start_due(t0 + seconds(33), true, 1), "retry waits its short delay");
  require(schedule.start_due(t0 + seconds(34), true, 1), "retry starts after the short delay");
  require(schedule.finish(CardIdentityRecheck::unreadable, t0 + seconds(35), true, 1) ==
              CardIdentityVerdict::lost, "second unreadable is lost");

  // A card event between start and finish voids the result and the count.
  require(!schedule.start_due(t0 + seconds(40), true, 2), "lost re-arms a full interval");
  require(schedule.start_due(t0 + seconds(70), true, 2), "due again");
  require(schedule.finish(CardIdentityRecheck::different_card, t0 + seconds(71), true, 3) ==
              CardIdentityVerdict::skipped, "result from an older card epoch decides nothing");
  require(!schedule.start_due(t0 + seconds(72), true, 3), "stale result re-arms the interval");
  require(schedule.start_due(t0 + seconds(102), true, 3), "due under the new epoch");
  require(schedule.finish(CardIdentityRecheck::different_card, t0 + seconds(103), false, 3) ==
              CardIdentityVerdict::skipped, "result after the session ended decides nothing");

  // Leaving eligibility clears a pending unreadable.
  require(!schedule.start_due(t0 + seconds(104), true, 3), "arms");
  require(schedule.start_due(t0 + seconds(134), true, 3), "due");
  require(schedule.finish(CardIdentityRecheck::unreadable, t0 + seconds(135), true, 3) ==
              CardIdentityVerdict::retry, "pending unreadable");
  require(!schedule.start_due(t0 + seconds(136), false, 3), "ineligible tick");
  require(!schedule.start_due(t0 + seconds(137), true, 3), "eligible again re-arms");
  require(schedule.start_due(t0 + seconds(167), true, 3), "due after a full interval");
  require(schedule.finish(CardIdentityRecheck::unreadable, t0 + seconds(168), true, 3) ==
              CardIdentityVerdict::retry, "pending unreadable was cleared");

  schedule.abandon(t0 + seconds(169));
  require(!schedule.start_due(t0 + seconds(170), true, 3), "abandon waits a full interval");
  require(schedule.start_due(t0 + seconds(199), true, 3), "due after an abandoned start");
}

void failures_keep_credentials_apart() {
  for (const auto error : {CardError::authentication_failed, CardError::authentication_locked,
                           CardError::api_key_invalid, CardError::api_key_missing,
                           CardError::api_key_calculation_failed,
                           CardError::api_key_verification_failed, CardError::user_sn_invalid}) {
    require(classify_identity_recheck_failure(make_error_code(error)) ==
                CardIdentityRecheck::credentials_rejected,
            "credential failure classified as rejected");
  }
  for (const auto error : {CardError::device_not_found, CardError::communication_failed,
                           CardError::operation_timed_out, CardError::multiple_devices,
                           CardError::backend_unavailable, CardError::device_failure}) {
    require(classify_identity_recheck_failure(make_error_code(error)) ==
                CardIdentityRecheck::unreadable,
            "device failure classified as unreadable");
  }
  require(classify_identity_recheck_failure(
              std::error_code(static_cast<int>(CardError::authentication_failed),
                              std::system_category())) == CardIdentityRecheck::unreadable,
          "only the card category can mean refused credentials");
}

void recheck_refused_while_foreground_in_flight() {
  CardRecheckCoordinator coordinator;
  {
    const CardForegroundOperation foreground(coordinator, std::chrono::milliseconds(0));
    require(!coordinator.try_begin_recheck(), "recheck must not start under a foreground operation");
  }
  require(coordinator.try_begin_recheck(), "recheck starts once the foreground ends");
  require(!coordinator.try_begin_recheck(), "only one recheck at a time");
  coordinator.end_recheck();
}

void foreground_waits_for_recheck_and_recheck_thread_passes() {
  CardRecheckCoordinator coordinator;
  std::atomic<bool> started{false};
  std::atomic<bool> nested_passed{false};
  std::atomic<bool> released{false};
  std::thread recheck([&] {
    require(coordinator.try_begin_recheck(), "recheck begins");
    {
      // The recheck's own helper call must not wait on itself.
      const CardForegroundOperation nested(coordinator, std::chrono::seconds(5));
      nested_passed = true;
    }
    started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    released = true;
    coordinator.end_recheck();
  });
  while (!started) std::this_thread::yield();
  const auto before = std::chrono::steady_clock::now();
  {
    const CardForegroundOperation foreground(coordinator, std::chrono::seconds(5));
    require(released.load(), "foreground waited for the recheck to end");
  }
  recheck.join();
  require(nested_passed.load(), "nested call on the recheck thread passed");
  require(std::chrono::steady_clock::now() - before < std::chrono::seconds(4),
          "foreground did not wait out the whole bound");
}

void foreground_proceeds_when_bound_expires() {
  CardRecheckCoordinator coordinator;
  std::atomic<bool> begun{false};
  std::atomic<bool> finish{false};
  std::thread recheck([&] {
    require(coordinator.try_begin_recheck(), "recheck begins");
    begun = true;
    while (!finish) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    coordinator.end_recheck();
  });
  while (!begun) std::this_thread::yield();
  {
    const CardForegroundOperation foreground(coordinator, std::chrono::milliseconds(50));
    // Still counted, so no new recheck can start behind it.
  }
  finish = true;
  recheck.join();
  require(coordinator.try_begin_recheck(), "foreground count returned to zero");
  coordinator.end_recheck();
}

}  // namespace

int main() {
  try {
    policy_retries_one_unreadable_then_counts_removal();
    policy_loses_at_once_on_other_card_or_refused_credentials();
    schedule_paces_retries_and_discards_stale_results();
    failures_keep_credentials_apart();
    recheck_refused_while_foreground_in_flight();
    foreground_waits_for_recheck_and_recheck_thread_passes();
    foreground_proceeds_when_bound_expires();
  } catch (const std::exception& error) {
    std::cerr << "card_identity_recheck_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "card_identity_recheck_tests passed\n";
  return 0;
}
