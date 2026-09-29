#pragma once

#include "usb_vault/application/product_settings_v1.h"
#include <array>
#include <cstdint>

namespace usb_vault::ui {

// No text, scan codes or input history: only physical down state and at most
// ten timestamps for the selected key. The complete sequence fits in 1 second.
class ExitHotkeyCounter final {
 public:
  explicit ExitHotkeyCounter(application::ExitHotkeySettings settings) : settings_(settings) {}
  void held(unsigned int key) noexcept { if (key < down_.size()) down_[key] = true; }
  bool input(unsigned int key, bool down, bool injected, std::uint64_t now, bool suppressed = false) noexcept {
    if (injected || key >= down_.size()) return false;
    const bool repeated = down_[key];
    down_[key] = down;
    if (suppressed) { size_ = 0; return false; }
    if (!down) return false;
    if (key != settings_.key) { size_ = 0; return false; }
    if (repeated || !settings_.enabled || !application::valid_exit_hotkey(settings_)) return false;
    for (const auto modifier : {0x10U, 0x11U, 0x12U, 0x5BU, 0x5CU, 0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U}) {
      if (down_[modifier]) { size_ = 0; return false; }
    }
    while (size_ && (now < times_[0] || now - times_[0] > 1000)) {
      for (unsigned int i = 1; i < size_; ++i) times_[i - 1] = times_[i];
      --size_;
    }
    times_[size_++] = now;
    if (size_ < settings_.count) return false;
    size_ = 0;
    return true;
  }
 private:
  application::ExitHotkeySettings settings_;
  std::array<bool, 256> down_{};
  std::array<std::uint64_t, 10> times_{};
  unsigned int size_{};
};

class IUiExitHotkeySink {
 public:
  virtual ~IUiExitHotkeySink() = default;
  virtual void set_capture(bool) noexcept {}
  virtual application::ExitHotkeySettings exit_hotkey() const noexcept = 0;
  virtual bool set_exit_hotkey(application::ExitHotkeySettings settings) noexcept = 0;
};
} // namespace usb_vault::ui
