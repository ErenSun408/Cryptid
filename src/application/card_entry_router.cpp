#include "usb_vault/application/card_entry_router.h"
#include "card/capture_failure.h"

namespace usb_vault::application {

CardEntryRoute CardEntryRouter::route(
    IReadOnlyCardEntryInspection& inspection,
    card::FailureDiagnostic* failure) noexcept {
  if (failure) *failure = {};
  const auto unavailable = [failure](const std::uint32_t code) {
    if (failure) failure->primary.code = code;
    return CardEntryRoute::unavailable;
  };
  try {
    switch (inspection.inspect_media()) {
      case CardEntryMediaEvidence::initializable_media:
        return CardEntryRoute::initialization_required;
      case CardEntryMediaEvidence::blocked:
        return unavailable(4001);
      case CardEntryMediaEvidence::product_record:
        break;
      default:
        return unavailable(4005);
    }

    switch (inspection.inspect_product_record_read_only()) {
      case card::CardRecordState::provisioned_uninitialized:
        return CardEntryRoute::initialization_required;
      case card::CardRecordState::initialized_v1:
        return CardEntryRoute::login_required;
      case card::CardRecordState::legacy_p1_blob:
        return unavailable(4001);
      case card::CardRecordState::unsupported_version:
        return unavailable(4002);
      case card::CardRecordState::malformed:
        return unavailable(4003);
      default:
        return unavailable(4005);
    }
  } catch (...) {
    if (failure) *failure = card::capture_failure(std::current_exception());
    return CardEntryRoute::unavailable;
  }
}

}  // namespace usb_vault::application
