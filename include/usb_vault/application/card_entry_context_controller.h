#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_router.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/card/card_initialization_service.h"
#include "usb_vault/card/card_login_target.h"
#include "usb_vault/card/card_update_target.h"

#include "usb_vault/application/card_operation_lease.h"
#include "usb_vault/card/card_reset_target.h"
#include "usb_vault/application/card_reset_confirmation.h"
#include <memory>
#include <optional>

namespace usb_vault::application {

// Vendor-neutral source for one adapter-authenticated physical insertion.
// Implementations may perform a one-shot helper operation, but must not write
// the card or retain secret credentials after this call returns.
class ICardEntryInspectionSource {
 public:
  virtual ~ICardEntryInspectionSource() noexcept = default;

  ICardEntryInspectionSource(const ICardEntryInspectionSource&) = delete;
  ICardEntryInspectionSource& operator=(
      const ICardEntryInspectionSource&) = delete;
  ICardEntryInspectionSource(ICardEntryInspectionSource&&) = delete;
  ICardEntryInspectionSource& operator=(ICardEntryInspectionSource&&) =
      delete;

  [[nodiscard]] virtual std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() = 0;

  // Production adapters coordinate all transactions and nested helper calls.
  [[nodiscard]] virtual std::unique_ptr<ICardOperationLease> acquire_operation() { return nullptr; }

  // Creates a short-lived write target bound to the exact read-only context
  // previously returned by this source. Implementations must not perform card
  // I/O or load credentials while creating the target. Unsupported sources
  // fail closed by returning null.
  [[nodiscard]] virtual std::unique_ptr<card::ICardInitializationTarget>
  make_initialization_target(
      const IReadOnlyCardEntryInspection&) {
    return nullptr;
  }

  // Creates a read-only login target bound to the exact initialized context
  // previously returned by this source. Construction must not load credentials
  // or perform card I/O. Unsupported sources fail closed by returning null.
  [[nodiscard]] virtual std::unique_ptr<card::ICardLoginTarget>
  make_login_target(const IReadOnlyCardEntryInspection&) {
    return nullptr;
  }

  // Creates a read/write update target bound to the exact initialized context
  // previously returned by this source. It exists only for replacing both
  // record envelopes; the login target stays read-only. Construction must not
  // load credentials or perform card I/O, and unsupported sources fail closed
  // by returning null.
  [[nodiscard]] virtual std::unique_ptr<card::ICardUpdateTarget>
  make_update_target(const IReadOnlyCardEntryInspection&) {
    return nullptr;
  }

  [[nodiscard]] virtual std::unique_ptr<card::ICardResetTarget>
  make_reset_target(const IReadOnlyCardEntryInspection&) { return nullptr; }

 protected:
  ICardEntryInspectionSource() noexcept = default;
};

// Secret-consuming product actions exposed to a native UI controller. This
// deliberately omits arrival/removal, insertion context, vendor identity, and
// raw record access. Implementations own all retry and invalidation semantics.
class ICardEntryActions {
 public:
  virtual ~ICardEntryActions() noexcept = default;

  ICardEntryActions(const ICardEntryActions&) = delete;
  ICardEntryActions& operator=(const ICardEntryActions&) = delete;
  ICardEntryActions(ICardEntryActions&&) = delete;
  ICardEntryActions& operator=(ICardEntryActions&&) = delete;

  virtual void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      security::ConfirmedPassword confirmed_daily_password) = 0;
  virtual void authenticate_login(
      ICardLoginSuccessConsumer& success_consumer,
      security::SecureBuffer login_password_utf8) = 0;
  // Replaces both envelopes of the current insertion's record so it opens
  // with the caller's existing password and with a second, newly chosen one.
  // Both inputs are consumed. The card layer records nothing about which
  // password is which.
  virtual void set_second_card_password(
      security::SecureBuffer existing_password_utf8,
      security::SecureBuffer second_password_utf8) = 0;
  // Re-inspects the physically present card and returns an initialized record
  // to the provisioning marker. The authenticated session is an explicit
  // authorization gate; the one-shot login context is never retained or
  // revived. The fresh insertion is retired on every outcome.
  virtual void reset_card_record(
      const IVolatileCardAuthenticationSession& authentication) = 0;
  // A login reset is offered for an initialized card, and for a card holding
  // a record written by an earlier, incompatible format, which can only be
  // reset and initialized again.
  [[nodiscard]] virtual bool can_login_reset() const noexcept {
    return current_route() == CardEntryRoute::login_required;
  }
  virtual std::uint32_t prepare_login_reset() { throw std::logic_error("login reset unsupported"); }
  virtual void cancel_login_reset() noexcept {}
  [[nodiscard]] virtual bool login_reset_pending(std::uint32_t) const noexcept { return false; }
  virtual void confirm_login_reset(std::uint32_t) { throw std::logic_error("login reset unsupported"); }
  [[nodiscard]] virtual std::optional<CardEntryRoute>
  current_route() const noexcept = 0;
  // Hardware identity of the present insertion while one is bound; nothing
  // after login or initialization has consumed it.
  [[nodiscard]] virtual std::optional<std::uint32_t>
  current_card_identity() const noexcept {
    return std::nullopt;
  }

 protected:
  ICardEntryActions() noexcept = default;
};

// Owns the strictly volatile context and fixed route for at most one physical
// insertion. Every arrival first destroys the previous binding; removal
// destroys the current binding before publishing the no-card state. Source
// failures and null contexts fail closed without an automatic retry.
class CardEntryContextController final : public ICardEntryActions {
 public:
  explicit CardEntryContextController(
      ICardEntryInspectionSource& source) noexcept;

  [[nodiscard]] CardEntryRoute handle_arrival() noexcept;
  void handle_removal() noexcept;

  // Consumes the current initialization route and context before invoking any
  // adapter or card operation, then moves the verified root directly into an
  // empty volatile authentication session. Success and every attempted card
  // operation leave this controller unusable until a physical removal and
  // fresh arrival establish a new read-only context.
  void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      security::ConfirmedPassword confirmed_daily_password) override;

  // Attempts one login against the exact initialized insertion. A credential
  // rejection preserves the route and context for another user attempt. Any
  // adapter, read, public-state, or cryptographic-system failure invalidates
  // the insertion. Success invalidates it before moving S and the original
  // login input into the synchronous non-UI consumer.
  void authenticate_login(
      ICardLoginSuccessConsumer& success_consumer,
      security::SecureBuffer login_password_utf8) override;
  void set_second_card_password(
      security::SecureBuffer existing_password_utf8,
      security::SecureBuffer second_password_utf8) override;
  void reset_card_record(
      const IVolatileCardAuthenticationSession& authentication) override;

  bool can_login_reset() const noexcept override;
  std::uint32_t prepare_login_reset() override;
  void cancel_login_reset() noexcept override;
  bool login_reset_pending(std::uint32_t ticket) const noexcept override;
  void confirm_login_reset(std::uint32_t ticket) override;

  // No value means no accepted physical insertion is currently present.
  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override;
  [[nodiscard]] std::optional<std::uint32_t>
  current_card_identity() const noexcept override;
  [[nodiscard]] bool has_context() const noexcept;
  // Last insertion's read-only failure, cleared on removal or next arrival.
  [[nodiscard]] card::FailureDiagnostic startup_failure() const noexcept {
    return startup_failure_;
  }

 private:
  ICardEntryInspectionSource& source_;
  std::unique_ptr<IReadOnlyCardEntryInspection> context_;
  std::optional<CardEntryRoute> route_;
  card::FailureDiagnostic startup_failure_{};
  CardResetConfirmation reset_confirmation_;
  std::unique_ptr<card::ICardResetTarget> reset_target_;
  security::SecureBuffer reset_snapshot_;
};

}  // namespace usb_vault::application
