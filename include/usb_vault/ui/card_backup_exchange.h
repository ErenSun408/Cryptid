#pragma once
#include "usb_vault/application/card_presence_controller.h"

namespace usb_vault::ui {
// A single intentional removal may suppress process exit, only after the
// ordinary lifecycle has revoked authentication and released every mount.
class CardBackupExchange final {
 public:
  bool allow_removal(application::CardPresenceTransition result) noexcept {
    using T = application::CardPresenceTransition;
    if (removed_ || (result != T::card_unavailable &&
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
