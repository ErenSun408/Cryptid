#pragma once

#include "usb_vault/application/card_entry_router.h"

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

  // No value means no accepted physical insertion is currently present.
  [[nodiscard]] std::optional<CardEntryRoute> current_route() const noexcept;
  [[nodiscard]] bool has_context() const noexcept;

 private:
  ICardEntryInspectionSource& source_;
  std::unique_ptr<IReadOnlyCardEntryInspection> context_;
  std::optional<CardEntryRoute> route_;
};

}  // namespace usb_vault::application
