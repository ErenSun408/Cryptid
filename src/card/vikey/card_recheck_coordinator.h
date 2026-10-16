#pragma once
#include "card/vikey/card_operation_lease.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace usb_vault::card::vikey {

// Orders the background identity recheck against this process's own card
// operations; it never changes the no-wait lease between instances. A
// foreground operation waits, within a bound, for an in-flight recheck to
// close instead of failing as busy, and a recheck never starts while a
// foreground operation is in flight. Both sides decide under one mutex, so a
// recheck cannot slip in between a foreground operation's wait and its lease.
class CardRecheckCoordinator final {
 public:
  CardRecheckCoordinator() = default;
  CardRecheckCoordinator(const CardRecheckCoordinator&) = delete;
  CardRecheckCoordinator& operator=(const CardRecheckCoordinator&) = delete;

  [[nodiscard]] static CardRecheckCoordinator& process() noexcept {
    static CardRecheckCoordinator value;
    return value;
  }

  // False when a foreground operation or another recheck is in flight.
  [[nodiscard]] bool try_begin_recheck() noexcept {
    std::lock_guard lock(mutex_);
    if (foreground_ != 0 || recheck_thread_ != 0) return false;
    recheck_thread_ = ::GetCurrentThreadId();
    return true;
  }

  void end_recheck() noexcept {
    {
      std::lock_guard lock(mutex_);
      recheck_thread_ = 0;
    }
    changed_.notify_all();
  }

  // The recheck's own nested helper call passes straight through. When the
  // bound expires the operation proceeds and meets the lease as before.
  [[nodiscard]] bool begin_foreground(
      const std::chrono::milliseconds bound) noexcept {
    std::unique_lock lock(mutex_);
    const DWORD self = ::GetCurrentThreadId();
    if (recheck_thread_ == self) return false;
    try {
      static_cast<void>(changed_.wait_for(
          lock, bound, [&] { return recheck_thread_ == 0; }));
    } catch (...) {
    }
    ++foreground_;
    return true;
  }

  void end_foreground() noexcept {
    std::lock_guard lock(mutex_);
    if (foreground_ != 0) --foreground_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  DWORD recheck_thread_{};
  std::uint32_t foreground_{};
};

class CardForegroundOperation final {
 public:
  explicit CardForegroundOperation(
      CardRecheckCoordinator& coordinator = CardRecheckCoordinator::process(),
      const std::chrono::milliseconds bound = std::chrono::seconds(10)) noexcept
      : coordinator_(coordinator), counted_(coordinator.begin_foreground(bound)) {}
  ~CardForegroundOperation() noexcept {
    if (counted_) coordinator_.end_foreground();
  }
  CardForegroundOperation(const CardForegroundOperation&) = delete;
  CardForegroundOperation& operator=(const CardForegroundOperation&) = delete;

 private:
  CardRecheckCoordinator& coordinator_;
  bool counted_;
};

// The instance lease for a foreground transaction that takes it directly
// rather than through a helper call: it first lets an in-flight recheck close.
class ForegroundCardOperationLease final : public application::ICardOperationLease {
 public:
  ForegroundCardOperationLease() = default;
  ForegroundCardOperationLease(const ForegroundCardOperationLease&) = delete;
  ForegroundCardOperationLease& operator=(const ForegroundCardOperationLease&) = delete;

 private:
  CardForegroundOperation foreground_;
  CardOperationLease lease_;
};

}  // namespace usb_vault::card::vikey
