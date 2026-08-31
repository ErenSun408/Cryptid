#pragma once

#include "usb_vault/application/active_volume_session.h"

namespace usb_vault::application {

// Non-secret, process-local result of one physical card-presence transition.
// It deliberately does not classify a session, volume, or user mode.
enum class CardPresenceTransition {
  no_change,
  card_available,
  card_unavailable,
  active_session_unmounted,
  active_session_force_unmounted,
  active_session_remains,
};

// Routes vendor-neutral card arrival/removal events to the active application
// session. Arrival never authenticates. First removal makes the card
// unavailable and unmounts an active session, escalating to a forced unmount
// when a drive is busy: the card behind the key material is physically gone,
// so leaving a decrypted drive letter behind is the worse outcome. A forced
// unmount is reported separately because it can discard unwritten data. Only
// a drive that even the forced unmount cannot release remains, and it stays
// retryable through the unchanged session state.
class CardPresenceController final {
 public:
  explicit CardPresenceController(
      IActiveVolumeSession& session,
      bool initially_present = false) noexcept;

  [[nodiscard]] CardPresenceTransition handle_arrival() noexcept;
  [[nodiscard]] CardPresenceTransition handle_removal() noexcept;
  [[nodiscard]] bool card_present() const noexcept;

 private:
  IActiveVolumeSession& session_;
  bool card_present_{};
};

}  // namespace usb_vault::application
