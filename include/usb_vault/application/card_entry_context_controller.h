#pragma once

#include "usb_vault/application/card_entry_router.h"
#include "usb_vault/card/card_initialization_service.h"

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

  // Creates a short-lived write target bound to the exact read-only context
  // previously returned by this source. Implementations must not perform card
  // I/O or load credentials while creating the target. Unsupported sources
  // fail closed by returning null.
  [[nodiscard]] virtual std::unique_ptr<card::ICardInitializationTarget>
  make_initialization_target(
      const IReadOnlyCardEntryInspection&) {
    return nullptr;
  }

 protected:
  ICardEntryInspectionSource() noexcept = default;
};

// Owns the strictly volatile context and fixed route for at most one physical
// insertion. Every arrival first destroys the previous binding; removal
// destroys the current binding before publishing the no-card state. Source
// failures and null contexts fail closed without an automatic retry.
class CardEntryContextController final {
 public:
  explicit CardEntryContextController(
      ICardEntryInspectionSource& source) noexcept;

  [[nodiscard]] CardEntryRoute handle_arrival() noexcept;
  void handle_removal() noexcept;

  // Consumes the current initialization route and context before invoking any
  // adapter or card operation. Success and every failure leave this controller
  // unusable until a physical removal and fresh arrival establish a new
  // read-only context.
  [[nodiscard]] security::SecureBuffer initialize_and_authenticate(
      security::ConfirmedPassword confirmed_daily_password);

  // No value means no accepted physical insertion is currently present.
  [[nodiscard]] std::optional<CardEntryRoute> current_route() const noexcept;
  [[nodiscard]] bool has_context() const noexcept;

 private:
  ICardEntryInspectionSource& source_;
  std::unique_ptr<IReadOnlyCardEntryInspection> context_;
  std::optional<CardEntryRoute> route_;
};

}  // namespace usb_vault::application
