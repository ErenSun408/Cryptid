#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_presence_controller.h"

#include <optional>

namespace usb_vault::application {

// Public, vendor-neutral outcome of one accepted physical device event. The
// optional route is empty after removal; entry_changed tells the UI whether
// it must refresh even when the mount transition itself was no_change.
struct CardDeviceLifecycleTransition final {
  CardPresenceTransition presence{CardPresenceTransition::no_change};
  std::optional<CardEntryRoute> entry_route;
  bool entry_changed{};
};

// Coordinates P2 entry-context lifetime with the existing P1.7 presence and
// forced-unmount semantics. Arrival marks presence, clears stale volatile
// authentication, then performs one read-only inspection. Duplicate arrival
// is ignored. Removal always invalidates the entry context, clears the root
// secret, and only then attempts to unmount an active session.
class CardDeviceLifecycleController final {
 public:
  CardDeviceLifecycleController(
      CardPresenceController& presence,
      CardEntryContextController& entry,
      IVolatileCardAuthenticationSession& authentication) noexcept;

  [[nodiscard]] CardDeviceLifecycleTransition handle_arrival() noexcept;
  // Controlled exchange: revoke old context and track arrival without SDK work.
  [[nodiscard]] CardDeviceLifecycleTransition handle_uninspected_arrival() noexcept;
  [[nodiscard]] CardDeviceLifecycleTransition handle_removal() noexcept;
  [[nodiscard]] std::optional<CardEntryRoute> current_route() const noexcept;

 private:
  CardPresenceController& presence_;
  CardEntryContextController& entry_;
  IVolatileCardAuthenticationSession& authentication_;
};

}  // namespace usb_vault::application
