#include "usb_vault/application/card_entry_router.h"

namespace usb_vault::application {

CardEntryRoute CardEntryRouter::route(
    IReadOnlyCardEntryInspection& inspection) noexcept {
  try {
    switch (inspection.inspect_media()) {
      case CardEntryMediaEvidence::initializable_media:
        return CardEntryRoute::initialization_required;
      case CardEntryMediaEvidence::blocked:
        return CardEntryRoute::unavailable;
      case CardEntryMediaEvidence::product_record:
        break;
      default:
        return CardEntryRoute::unavailable;
    }

    switch (inspection.inspect_product_record_read_only()) {
      case card::CardRecordState::provisioned_uninitialized:
        return CardEntryRoute::initialization_required;
      case card::CardRecordState::initialized_v1:
        return CardEntryRoute::login_required;
      case card::CardRecordState::legacy_p1_blob:
      case card::CardRecordState::unsupported_version:
      case card::CardRecordState::malformed:
      default:
        return CardEntryRoute::unavailable;
    }
  } catch (...) {
    return CardEntryRoute::unavailable;
  }
}

}  // namespace usb_vault::application
