#pragma once
#include <cstdint>

namespace usb_vault::ui {
// UI-thread state. Every navigation gets a new generation; late callbacks
// cannot mark the replacement page ready or report its predecessor's error.
class WebviewStartupState final {
 public:
  void begin(std::uint64_t generation, std::uint64_t now) noexcept {
    generation_ = generation; started_ = now;
    active_ = true; ready_ = false; failed_ = false;
  }
  void stop() noexcept { active_ = false; }
  [[nodiscard]] bool current(std::uint64_t generation) const noexcept {
    return active_ && !failed_ && generation == generation_;
  }
  [[nodiscard]] bool fail(std::uint64_t generation) noexcept {
    if (!current(generation)) return false;
    failed_ = true;
    return true;
  }
  bool ready(std::uint64_t generation) noexcept {
    if (!current(generation)) return false;
    ready_ = true;
    return true;
  }
  [[nodiscard]] bool expired(std::uint64_t generation,
                             std::uint64_t now) const noexcept {
    return current(generation) && !ready_ && now - started_ >= 30000;
  }
 private:
  std::uint64_t generation_{};
  std::uint64_t started_{};
  bool active_{};
  bool ready_{};
  bool failed_{};
};
}  // namespace usb_vault::ui
