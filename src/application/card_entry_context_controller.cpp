#include "usb_vault/application/card_entry_context_controller.h"

#include "usb_vault/card/card_error.h"

#include <system_error>
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

void CardEntryContextController::initialize_and_authenticate(
    CardAuthenticationSession& authentication,
    security::ConfirmedPassword confirmed_daily_password) {
  if (route_ != CardEntryRoute::initialization_required || !context_ ||
      authentication.authenticated()) {
    throw std::system_error(
        card::make_error_code(card::CardError::invalid_operation),
        "card initialization requires a fresh eligible insertion");
  }

  route_ = CardEntryRoute::unavailable;
  std::unique_ptr<IReadOnlyCardEntryInspection> context =
      std::move(context_);
  std::unique_ptr<card::ICardInitializationTarget> target =
      source_.make_initialization_target(*context);
  if (!target) {
    throw std::system_error(
        card::make_error_code(card::CardError::backend_unavailable),
        "card adapter cannot initialize this insertion");
  }

  authentication.establish(
      card::CardInitializationService::initialize_and_authenticate(
          *target, std::move(confirmed_daily_password)));
}

std::optional<CardEntryRoute>
CardEntryContextController::current_route() const noexcept {
  return route_;
}

bool CardEntryContextController::has_context() const noexcept {
  return context_ != nullptr;
}

}  // namespace usb_vault::application
