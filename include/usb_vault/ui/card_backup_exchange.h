#pragma once
#include "usb_vault/application/card_presence_controller.h"
#include "usb_vault/card/card_error.h"
#include <chrono>
#include <cstdint>
#include <system_error>

namespace usb_vault::ui {
inline bool retryable_card_backup_probe(const std::error_code& error) noexcept {
  using card::CardError;
  return error == CardError::device_not_found || error == CardError::communication_failed ||
         error == CardError::operation_timed_out;
}
// A timeout in the probe, write or read-back ends the helper mid-session, so
// the spare card may be left unfinished. It must not be reported as a source,
// blank or credential problem.
inline const wchar_t* card_backup_failure_text(const std::error_code& error) noexcept {
  if (error == card::CardError::operation_timed_out)
    return L"备卡操作超时，备卡未完成。\n请拔出备用卡后重新开始备卡；若该卡显示为非空白，请改用另一张空白卡。";
  return L"备卡未完成。请确认原卡记录完整、备用卡为空白且供应方凭据有效。";
}
// Retry transient read failures without overlapping workers or busy polling.
class CardBackupProbeRetry final {
 public:
  using Clock = std::chrono::steady_clock;
  void reset() noexcept { next_ = {}; delay_ = 1; }
  bool due(Clock::time_point now = Clock::now()) const noexcept { return now >= next_; }
  void failed(Clock::time_point now = Clock::now()) noexcept {
    next_ = now + std::chrono::seconds(delay_);
    if (delay_ < 4) delay_ *= 2;
  }
 private:
  Clock::time_point next_{};
  unsigned int delay_{1};
};
// An async result belongs to one uninterrupted, settled physical connection.
class CardBackupTargetReadiness final {
 public:
  using Clock = std::chrono::steady_clock;
  void invalidate() noexcept { ++generation_; present_ = false; since_ = {}; }
  bool observe(bool present, Clock::time_point now = Clock::now()) noexcept {
    if (!present && present_) { invalidate(); return true; }
    if (present && !present_) { present_ = true; since_ = now; }
    return false;
  }
  bool stable(Clock::time_point now = Clock::now()) const noexcept {
    return present_ && now - since_ >= std::chrono::milliseconds(600);
  }
  std::uint64_t generation() const noexcept { return generation_; }
  bool accepts(std::uint64_t generation, Clock::time_point now = Clock::now()) const noexcept {
    return generation == generation_ && stable(now);
  }
 private:
  std::uint64_t generation_{};
  bool present_{};
  Clock::time_point since_{};
};

// Waiting for a spare may tolerate repeated disconnects only when explicitly
// requested, after ordinary lifecycle cleanup. Never use retries during writes.
class CardBackupExchange final {
 public:
  bool allow_removal(application::CardPresenceTransition result, bool waiting_retry = false) noexcept {
    using T = application::CardPresenceTransition;
    if ((removed_ && !waiting_retry) || (result != T::card_unavailable &&
                    result != T::active_session_unmounted &&
                    result != T::active_session_force_unmounted)) return false;
    removed_ = true;
    return true;
  }
  bool removed() const noexcept { return removed_; }
 private:
  bool removed_{};
};
}  // namespace usb_vault::ui
