#pragma once

#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/card/card_product_record_state.h"
#include "usb_vault/card/failure_diagnostic.h"

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
  // Known interrupted v2 footprints permit only a newly confirmed destruction.
  [[nodiscard]] virtual bool destructive_reset_available() const noexcept { return false; }

  // Legacy classification used by the default supported-format implementation,
  // exactly once and only after inspect_media() reports product_record.
  // This method must classify the selected logical record without repairing,
  // normalizing, or otherwise writing the card.
  [[nodiscard]] virtual card::CardRecordState
  inspect_product_record_read_only() = 0;

  // New adapters explicitly opt in after validating the selected v2 journal.
  // Existing adapters retain their exact legacy interpretation; unsupported
  // evidence cannot implicitly become an initialized v2 route.
  [[nodiscard]] virtual card::CardProductRecordState
  inspect_supported_product_record_read_only() {
    return card::product_state_from_legacy(inspect_product_record_read_only());
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
      card::FailureDiagnostic* failure = nullptr,
      card::CardProductRecordState* product_state = nullptr) noexcept;
};

}  // namespace usb_vault::application
