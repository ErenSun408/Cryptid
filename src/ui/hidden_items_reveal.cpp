#include "usb_vault/ui/hidden_items_reveal.h"

#include <Windows.h>
#include <ShlObj.h>

namespace usb_vault::ui {
namespace {

constexpr HiddenItemsVisibility kRevealed{true, true};

// `SHGetSetSettings` writes the same values Explorer's folder options write,
// and refreshes this process's cached copy, so a dialog shown afterwards
// enumerates with them.
class ShellHiddenItemsSettings final : public IHiddenItemsSettings {
 public:
  [[nodiscard]] HiddenItemsVisibility read() noexcept override {
    SHELLSTATE state{};
    ::SHGetSetSettings(&state, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
    return {state.fShowAllObjects != FALSE, state.fShowSuperHidden != FALSE};
  }

  void write(const HiddenItemsVisibility visibility) noexcept override {
    SHELLSTATE state{};
    state.fShowAllObjects = visibility.hidden_items ? TRUE : FALSE;
    state.fShowSuperHidden = visibility.protected_system_items ? TRUE : FALSE;
    ::SHGetSetSettings(&state, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, TRUE);
  }
};

}  // namespace

HiddenItemsRevealer::Scope HiddenItemsRevealer::reveal() noexcept {
  const std::lock_guard lock(mutex_);
  if (active_++ == 0) {
    original_ = settings_.read();
    restore_ = original_ != kRevealed;
    if (restore_) settings_.write(kRevealed);
  }
  return Scope(*this);
}

void HiddenItemsRevealer::release() noexcept {
  const std::lock_guard lock(mutex_);
  if (active_ == 0 || --active_ != 0 || !restore_) return;
  restore_ = false;
  if (settings_.read() == kRevealed) settings_.write(original_);
}

HiddenItemsRevealer& shell_hidden_items_revealer() noexcept {
  static ShellHiddenItemsSettings settings;
  static HiddenItemsRevealer revealer(settings);
  return revealer;
}

}  // namespace usb_vault::ui
