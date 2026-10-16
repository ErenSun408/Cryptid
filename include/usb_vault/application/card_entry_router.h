#pragma once

#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/card/failure_diagnostic.h"

#include <cstdint>
#include <optional>

namespace usb_vault::application {

// Vendor-neutral evidence from a strictly read-only inspection of one
// physical insertion context. An adapter may report initializable_media only
// after proving its own device identity, accessibility, complete media read,
// and absence of legacy, foreign, conflicting, or damaged product evidence.
enum class CardEntryMediaEvidence {
  initializable_media,
  product_record,
  blocked,
};

// The only product views selected from card evidence. unavailable deliberately
// combines unsupported media, damaged or future records, and inspection
// failures so the UI cannot turn detailed card evidence into a disclosure
// surface or an accidental initialization path.
enum class CardEntryRoute {
  initialization_required,
  login_required,
  unavailable,
};

// Read-only insertion boundary. In particular, this is separate from
// ICardStore::get_blob(): a recoverable store read may normalize its private
// journal and therefore is not suitable for deciding which page to display.
// Implementations must retain any hardware identity and vendor state only in
// their private insertion context.
class IReadOnlyCardEntryInspection {
 public:
  virtual ~IReadOnlyCardEntryInspection() noexcept = default;

  IReadOnlyCardEntryInspection(const IReadOnlyCardEntryInspection&) = delete;
  IReadOnlyCardEntryInspection& operator=(
      const IReadOnlyCardEntryInspection&) = delete;
  IReadOnlyCardEntryInspection(IReadOnlyCardEntryInspection&&) = delete;
  IReadOnlyCardEntryInspection& operator=(IReadOnlyCardEntryInspection&&) =
      delete;

  [[nodiscard]] virtual CardEntryMediaEvidence inspect_media() = 0;

  // Called exactly once and only after inspect_media() reports product_record.
  // This method must classify the selected logical record without repairing,
  // normalizing, or otherwise writing the card.
  [[nodiscard]] virtual card::CardRecordState
  inspect_product_record_read_only() = 0;

  // The non-secret hardware identity of this insertion, used only to select
  // the card's activation grant. Adapters without one report nothing, which
  // the activation gate treats as not activated.
  [[nodiscard]] virtual std::optional<std::uint32_t> card_identity()
      const noexcept {
    return std::nullopt;
  }

 protected:
  IReadOnlyCardEntryInspection() noexcept = default;
};

class CardEntryRouter final {
 public:
  // Converts one insertion inspection into a fixed UI route. Every exception
  // and every unknown enum value fails closed to unavailable.
  [[nodiscard]] static CardEntryRoute route(
      IReadOnlyCardEntryInspection& inspection,
      card::FailureDiagnostic* failure = nullptr) noexcept;
};

}  // namespace usb_vault::application
