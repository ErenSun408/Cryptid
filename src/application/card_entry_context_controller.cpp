#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "card/capture_failure.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_service.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/card/card_record_v2.h"

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
    ICardEntryInspectionSource& source, std::function<bool()> confirm_password_update) noexcept
    : source_(source), confirm_password_update_(std::move(confirm_password_update)) {}

CardEntryRoute CardEntryContextController::handle_arrival() noexcept {
  entry_format_ = card::CardProductRecordState::malformed;
  cancel_login_reset();
  startup_failure_ = {};
  context_.reset();
  route_ = CardEntryRoute::unavailable;

  try {
    std::unique_ptr<IReadOnlyCardEntryInspection> candidate =
        source_.inspect_inserted_card();
    if (!candidate) {
      startup_failure_.primary.code = 4004;
      return *route_;
    }

    route_ = CardEntryRouter::route(*candidate, &startup_failure_, &entry_format_);
    context_ = std::move(candidate);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("application/card_entry_context_controller", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    startup_failure_ = card::capture_failure(std::current_exception());
  }
  return *route_;
}

void CardEntryContextController::handle_removal() noexcept {
  entry_format_ = card::CardProductRecordState::malformed;
  cancel_login_reset();
  startup_failure_ = {};
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

  cancel_login_reset();
  auto operation_lease = source_.acquire_operation();

  // Retired up front, and left retired on every failure below: a card whose
  // initialization tore halfway is in a state nobody knows, and no retry may
  // run against it.
  route_ = CardEntryRoute::unavailable;
  entry_format_ = card::CardProductRecordState::malformed;
  std::unique_ptr<IReadOnlyCardEntryInspection> context =
      std::move(context_);
  std::unique_ptr<card::ICardInitializationTarget> target =
      source_.make_initialization_target(*context);
  if (!target) {
    throw std::system_error(
        card::make_error_code(card::CardError::backend_unavailable),
        "card adapter cannot initialize this insertion");
  }

  auto root =
      card::CardInitializationService::initialize_and_authenticate(
          *target, std::move(confirmed_daily_password));
  target.reset();
  context.reset();
  authentication.establish(std::move(root));
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
  cancel_login_reset();
  auto operation_lease = source_.acquire_operation();

  // Login success deliberately destroys its one-shot insertion context. A
  // later record update starts from a new read-only inspection and binds its
  // write target to the card that is physically present at that moment.
  route_ = CardEntryRoute::unavailable;
  entry_format_ = card::CardProductRecordState::malformed;
  context_.reset();
  std::unique_ptr<IReadOnlyCardEntryInspection> context;
  std::unique_ptr<card::ICardUpdateTarget> target;
  try {
    context = source_.inspect_inserted_card();
    auto format = card::CardProductRecordState::malformed;
    if (!context || CardEntryRouter::route(*context, nullptr, &format) !=
                        CardEntryRoute::login_required) {
      throw std::system_error(
          card::make_error_code(card::CardError::invalid_operation),
          "card record update requires a fresh initialized insertion");
    }
    target = source_.make_password_update_target(*context);
    if (!target) {
      throw std::system_error(
          card::make_error_code(card::CardError::backend_unavailable),
          "card adapter cannot update this insertion");
    }
    if (target->record_format() != format) {
      throw std::system_error(card::make_error_code(card::CardError::invalid_operation));
    }
    target->append_password(std::move(existing_password_utf8),
                           std::move(second_password_utf8), confirm_password_update_);
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

  cancel_login_reset();
  auto operation_lease = source_.acquire_operation();

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

  cancel_login_reset();
  auto operation_lease = source_.acquire_operation();

  std::unique_ptr<card::ICardLoginTarget> target;
  auto format = card::CardProductRecordState::initialized_v1;
  security::SecureBuffer record;
  try {
    target = source_.make_login_target(*context_);
    if (!target) {
      throw std::system_error(
          card::make_error_code(card::CardError::backend_unavailable),
          "card adapter cannot authenticate this insertion");
    }
    format = target->record_format();
    if (format != entry_format_ || (format != card::CardProductRecordState::initialized_v1 &&
        format != card::CardProductRecordState::initialized_v2)) {
      throw std::system_error(card::make_error_code(card::CardError::invalid_operation), "unsupported login target format");
    }
    record = target->read_record();
  } catch (...) {
    target.reset();
    entry_format_ = card::CardProductRecordState::malformed;
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  security::SecureBuffer root_secret;
  try {
    if (format == card::CardProductRecordState::initialized_v1) {
      const auto state = card::inspect_card_record(record.bytes());
      if (state != card::CardRecordState::initialized_v1) throw card::UnexpectedCardRecordState(state);
    } else if (card::inspect_card_record_v2(record.bytes()) != card::CardRecordV2State::initialized) {
      throw std::system_error(card::make_error_code(card::CardError::invalid_operation), "invalid v2 login framing");
    }
    auto authentication_password = duplicate_secret(login_password_utf8);
    root_secret = format == card::CardProductRecordState::initialized_v1
        ? card::unlock_card_record_v1(record.bytes(), std::move(authentication_password))
        : card::unlock_card_record_v2(record.bytes(), std::move(authentication_password));
  } catch (const card::CardCredentialRejected&) {
    throw;
  } catch (...) {
    target.reset();
    entry_format_ = card::CardProductRecordState::malformed;
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
    // The final I/O must verify the anchor and exact logical record, including
    // authenticated recovery's complete postimage. A later unauthenticated read
    // must not hide an anchor-only change. Every gate error retires the context.
    if (format == card::CardProductRecordState::initialized_v2) {
      target->complete_authenticated_read(record.bytes(), root_secret);
    }
  } catch (...) {
    target.reset();
    entry_format_ = card::CardProductRecordState::malformed;
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }

  target.reset();
  record.clear();
  verified_record.clear();
  entry_format_ = card::CardProductRecordState::malformed;
  route_ = CardEntryRoute::unavailable;
  context_.reset();
  success_consumer.consume_authenticated_login(
      std::move(root_secret), std::move(login_password_utf8));
}


std::uint32_t CardEntryContextController::prepare_login_reset() {
  cancel_login_reset();
  if (route_ != CardEntryRoute::login_required || !context_)
    throw std::system_error(card::make_error_code(card::CardError::invalid_operation));
  try {
    auto lease = source_.acquire_operation();
    auto target = source_.make_reset_target(*context_);
    if (!target)
      throw std::system_error(card::make_error_code(card::CardError::backend_unavailable));
    auto snapshot = target->snapshot();
    if (card::inspect_card_record(snapshot.bytes()) != card::CardRecordState::initialized_v1)
      throw std::system_error(card::make_error_code(card::CardError::invalid_operation));
    const auto ticket = reset_confirmation_.prepare();
    reset_target_ = std::move(target);
    reset_snapshot_ = std::move(snapshot);
    return ticket;
  } catch (...) {
    cancel_login_reset();
    route_ = CardEntryRoute::unavailable;
    context_.reset();
    throw;
  }
}

void CardEntryContextController::cancel_login_reset() noexcept {
  reset_confirmation_.cancel();
  reset_target_.reset();
  reset_snapshot_.clear();
}

bool CardEntryContextController::login_reset_pending(const std::uint32_t ticket) const noexcept {
  return route_ == CardEntryRoute::login_required && context_ && reset_target_ &&
         reset_confirmation_.matches(ticket);
}

void CardEntryContextController::confirm_login_reset(const std::uint32_t ticket) {
  if (!login_reset_pending(ticket)) {
    cancel_login_reset();
    throw std::system_error(card::make_error_code(card::CardError::invalid_operation));
  }
  static_cast<void>(reset_confirmation_.consume(ticket));
  auto target = std::move(reset_target_);
  auto snapshot = std::move(reset_snapshot_);
  route_ = CardEntryRoute::unavailable;
  context_.reset();
  auto lease = source_.acquire_operation();
  target->reset(snapshot);
}

std::optional<CardEntryRoute>
CardEntryContextController::current_route() const noexcept {
  return route_;
}

bool CardEntryContextController::has_context() const noexcept {
  return context_ != nullptr;
}

}  // namespace usb_vault::application
