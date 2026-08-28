#include "usb_vault/application/card_presence_controller.h"

namespace usb_vault::application {

CardPresenceController::CardPresenceController(
    IApplicationSessionActions& session, const bool initially_present) noexcept
    : session_(session), card_present_(initially_present) {}

CardPresenceTransition CardPresenceController::handle_arrival() noexcept {
  if (card_present_) {
    return CardPresenceTransition::no_change;
  }
  card_present_ = true;
  return CardPresenceTransition::card_available;
}

CardPresenceTransition CardPresenceController::handle_removal() noexcept {
  if (!card_present_) {
    return CardPresenceTransition::no_change;
  }
  card_present_ = false;
  if (!session_.mounted()) {
    return CardPresenceTransition::card_unavailable;
  }
  bool forced = false;
  try {
    forced = session_.force_unmount_active();
  } catch (...) {
    return CardPresenceTransition::active_session_remains;
  }
  if (session_.mounted()) {
    return CardPresenceTransition::active_session_remains;
  }
  return forced ? CardPresenceTransition::active_session_force_unmounted
                : CardPresenceTransition::active_session_unmounted;
}

bool CardPresenceController::card_present() const noexcept {
  return card_present_;
}

}  // namespace usb_vault::application
