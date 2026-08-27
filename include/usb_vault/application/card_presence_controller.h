#pragma once

#include "usb_vault/application/application_session.h"

namespace usb_vault::application {

// Non-secret, process-local result of one physical card-presence transition.
// It deliberately does not classify a session, volume, or user mode.
enum class CardPresenceTransition {
  no_change,
  card_available,
  card_unavailable,
  active_session_unmounted,
  active_session_remains,
};

// Routes vendor-neutral card arrival/removal events to the active application
// session. Arrival never authenticates. First removal makes the card
// unavailable and attempts to unmount an active session; a failure remains
// retryable through the unchanged session state.
class CardPresenceController final {
 public:
  explicit CardPresenceController(
      IApplicationSessionActions& session,
      bool initially_present = false) noexcept;

  [[nodiscard]] CardPresenceTransition handle_arrival() noexcept;
  [[nodiscard]] CardPresenceTransition handle_removal() noexcept;
  [[nodiscard]] bool card_present() const noexcept;

 private:
  IApplicationSessionActions& session_;
  bool card_present_{};
};

}  // namespace usb_vault::application
