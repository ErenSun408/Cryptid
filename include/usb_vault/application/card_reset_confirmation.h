#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace usb_vault::application {
// UI-thread owned, volatile one-shot authorization. No identity or secrets cross
// the UI boundary. Owners must cancel on insertion and route changes.
class CardResetConfirmation final {
 public:
  [[nodiscard]] std::uint32_t prepare() {
    cancel();
    if (sequence_ == std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("reset confirmation sequence exhausted");
    pending_ = ++sequence_;
    return pending_;
  }
  void cancel() noexcept { pending_ = 0; }
  [[nodiscard]] bool matches(std::uint32_t ticket) const noexcept {
    return ticket != 0 && ticket == pending_;
  }
  [[nodiscard]] bool consume(std::uint32_t ticket) noexcept {
    const bool accepted = matches(ticket);
    cancel();
    return accepted;
  }
 private:
  std::uint32_t sequence_{};
  std::uint32_t pending_{};
};
}  // namespace usb_vault::application
