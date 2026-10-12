#include "usb_vault/application/card_device_lifecycle_controller.h"

namespace usb_vault::application {

CardDeviceLifecycleController::CardDeviceLifecycleController(
    CardPresenceController& presence,
    CardEntryContextController& entry,
    IVolatileCardAuthenticationSession& authentication) noexcept
    : presence_(presence),
      entry_(entry),
      authentication_(authentication) {}

CardDeviceLifecycleTransition
CardDeviceLifecycleController::handle_arrival() noexcept {
  const CardPresenceTransition presence = presence_.handle_arrival();
  if (presence != CardPresenceTransition::card_available) {
    return CardDeviceLifecycleTransition{
        .presence = presence,
        .entry_route = entry_.current_route(),
        .entry_changed = false,
    };
  }

  authentication_.clear_authentication();

  return CardDeviceLifecycleTransition{
      .presence = presence,
      .entry_route = entry_.handle_arrival(),
      .entry_changed = true,
  };
}

CardDeviceLifecycleTransition
CardDeviceLifecycleController::handle_uninspected_arrival() noexcept {
  const auto presence = presence_.handle_arrival();
  if (presence == CardPresenceTransition::card_available) {
    authentication_.clear_authentication();
    entry_.handle_removal();
  }
  return {.presence = presence, .entry_route = entry_.current_route(),
          .entry_changed = presence == CardPresenceTransition::card_available};
}

CardDeviceLifecycleTransition
CardDeviceLifecycleController::handle_removal() noexcept {
  const bool entry_changed = entry_.current_route().has_value();
  entry_.handle_removal();
  authentication_.clear_authentication();
  return CardDeviceLifecycleTransition{
      .presence = presence_.handle_removal(),
      .entry_route = std::nullopt,
      .entry_changed = entry_changed,
  };
}

std::optional<CardEntryRoute>
CardDeviceLifecycleController::current_route() const noexcept {
  return entry_.current_route();
}

}  // namespace usb_vault::application
