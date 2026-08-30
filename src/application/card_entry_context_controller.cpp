#include "usb_vault/application/card_entry_context_controller.h"

#include <utility>

namespace usb_vault::application {

CardEntryContextController::CardEntryContextController(
    ICardEntryInspectionSource& source) noexcept
    : source_(source) {}

CardEntryRoute CardEntryContextController::handle_arrival() noexcept {
  context_.reset();
  route_ = CardEntryRoute::unavailable;

  try {
    std::unique_ptr<IReadOnlyCardEntryInspection> candidate =
        source_.inspect_inserted_card();
    if (!candidate) return *route_;

    route_ = CardEntryRouter::route(*candidate);
    context_ = std::move(candidate);
  } catch (...) {
  }
  return *route_;
}

void CardEntryContextController::handle_removal() noexcept {
  context_.reset();
  route_.reset();
}

std::optional<CardEntryRoute>
CardEntryContextController::current_route() const noexcept {
  return route_;
}

bool CardEntryContextController::has_context() const noexcept {
  return context_ != nullptr;
}

}  // namespace usb_vault::application
