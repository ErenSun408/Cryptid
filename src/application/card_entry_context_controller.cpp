#include "usb_vault/application/card_entry_context_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_service.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <system_error>
#include <utility>

namespace usb_vault::application {
namespace {

[[nodiscard]] security::SecureBuffer duplicate_secret(
    const security::SecureBuffer& source) {
  // Card Record authentication consumes its input, while the exact NFC login
  // bytes are still needed for the immediate volume-password handoff.
  security::SecureBuffer duplicate(source.size());
  std::ranges::copy(source.bytes(), duplicate.bytes().begin());
  return duplicate;
}

[[nodiscard]] bool equal_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size()) return false;
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0;
}

}  // namespace

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

  // Retired up front, and left retired on every failure below: a card whose
  // initialization tore halfway is in a state nobody knows, and no retry may
  // run against it.
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

void CardEntryContextController::set_second_card_password(
    security::SecureBuffer existing_password_utf8,
    security::SecureBuffer second_password_utf8) {
  if (existing_password_utf8.empty() || second_password_utf8.empty()) {
    existing_password_utf8.clear();
    second_password_utf8.clear();
    throw std::invalid_argument(
        "both card record passwords are required");
  }
  // Login success deliberately destroys its one-shot insertion context. A
  // later record update starts from a new read-only inspection and binds its
  // write target to the card that is physically present at that moment.
  route_ = CardEntryRoute::unavailable;
  context_.reset();
  std::unique_ptr<IReadOnlyCardEntryInspection> context;
  std::unique_ptr<card::ICardUpdateTarget> target;
  try {
    context = source_.inspect_inserted_card();
    if (!context || CardEntryRouter::route(*context) !=
                        CardEntryRoute::login_required) {
      throw std::system_error(
          card::make_error_code(card::CardError::invalid_operation),
          "card record update requires a fresh initialized insertion");
    }
    target = source_.make_update_target(*context);
    if (!target) {
      throw std::system_error(
          card::make_error_code(card::CardError::backend_unavailable),
          "card adapter cannot update this insertion");
    }
    card::CardRecordService service(target->card_store());
    service.set_second_password(std::move(existing_password_utf8),
                                std::move(second_password_utf8));
  } catch (...) {
    existing_password_utf8.clear();
    second_password_utf8.clear();
    target.reset();
    // The card may hold either record after a failed write, so the insertion
    // is retired and the user reinserts instead of continuing on a context
    // whose state is no longer known.
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }
}

void CardEntryContextController::reset_card_record(
    const IVolatileCardAuthenticationSession& authentication) {
  if (!authentication.authenticated()) {
    throw std::system_error(
        card::make_error_code(card::CardError::invalid_operation),
        "card reset requires an authenticated session");
  }

  // Never revive the insertion consumed by login. The destructive operation
  // receives a new adapter-authenticated observation and binds the write to
  // that exact card.
  route_ = CardEntryRoute::unavailable;
  context_.reset();
  std::unique_ptr<IReadOnlyCardEntryInspection> context;
  std::unique_ptr<card::ICardUpdateTarget> target;
  try {
    context = source_.inspect_inserted_card();
    if (!context || CardEntryRouter::route(*context) !=
                        CardEntryRoute::login_required) {
      throw std::system_error(
          card::make_error_code(card::CardError::invalid_operation),
          "card reset requires a fresh initialized insertion");
    }
    target = source_.make_update_target(*context);
    if (!target) {
      throw std::system_error(
          card::make_error_code(card::CardError::backend_unavailable),
          "card adapter cannot update this insertion");
    }
    card::CardRecordService service(target->card_store());
    service.reset_to_provisioning();
  } catch (...) {
    target.reset();
    // The card may hold either record after a failed write, so the insertion
    // is retired and the user reinserts instead of continuing on a context
    // whose state is no longer known.
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  // A committed reset retires the insertion just as hard: this context was
  // routed as an initialized card, and that route is fixed for its lifetime.
  target.reset();
  route_ = CardEntryRoute::unavailable;
  context_.reset();
}

void CardEntryContextController::authenticate_login(
    ICardLoginSuccessConsumer& success_consumer,
    security::SecureBuffer login_password_utf8) {
  if (route_ != CardEntryRoute::login_required || !context_) {
    throw std::system_error(
        card::make_error_code(card::CardError::invalid_operation),
        "card login requires a fresh initialized insertion");
  }

  std::unique_ptr<card::ICardLoginTarget> target;
  security::SecureBuffer record;
  try {
    target = source_.make_login_target(*context_);
    if (!target) {
      throw std::system_error(
          card::make_error_code(card::CardError::backend_unavailable),
          "card adapter cannot authenticate this insertion");
    }
    record = target->read_record();
  } catch (...) {
    target.reset();
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  security::SecureBuffer root_secret;
  try {
    const card::CardRecordState state =
        card::inspect_card_record(record.bytes());
    if (state != card::CardRecordState::initialized_v1) {
      throw card::UnexpectedCardRecordState(state);
    }
    security::SecureBuffer authentication_password =
        duplicate_secret(login_password_utf8);
    root_secret = card::unlock_card_record_v1(
        record.bytes(), std::move(authentication_password));
  } catch (const card::CardCredentialRejected&) {
    throw;
  } catch (...) {
    target.reset();
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  security::SecureBuffer verified_record;
  try {
    verified_record = target->read_record();
    if (!equal_bytes(record.bytes(), verified_record.bytes())) {
      throw std::system_error(
          card::make_error_code(card::CardError::device_failure),
          "card record changed during login");
    }
  } catch (...) {
    target.reset();
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  target.reset();
  record.clear();
  verified_record.clear();
  route_ = CardEntryRoute::unavailable;
  context_.reset();
  success_consumer.consume_authenticated_login(
      std::move(root_secret), std::move(login_password_utf8));
}

std::optional<CardEntryRoute>
CardEntryContextController::current_route() const noexcept {
  return route_;
}

bool CardEntryContextController::has_context() const noexcept {
  return context_ != nullptr;
}

}  // namespace usb_vault::application
