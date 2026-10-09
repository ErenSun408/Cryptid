#include "usb_vault/application/card_entry_router.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "card/capture_failure.h"

namespace usb_vault::application {

CardEntryRoute CardEntryRouter::route(
    IReadOnlyCardEntryInspection& inspection,
    card::FailureDiagnostic* failure, card::CardProductRecordState* product_state) noexcept {
  if (failure) *failure = {};
  if (product_state) *product_state = card::CardProductRecordState::malformed;
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

    const auto state = inspection.inspect_supported_product_record_read_only();
    if (product_state) *product_state = state;
    switch (state) {
      case card::CardProductRecordState::provisioned_uninitialized:
        return CardEntryRoute::initialization_required;
      case card::CardProductRecordState::initialized_v1:
      case card::CardProductRecordState::initialized_v2:
        return CardEntryRoute::login_required;
      case card::CardProductRecordState::legacy_p1_blob:
        return unavailable(4001);
      case card::CardProductRecordState::unsupported_version:
        return unavailable(4002);
      case card::CardProductRecordState::malformed:
        return unavailable(4003);
      default:
        return unavailable(4005);
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("application/card_entry_router", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    if (failure) *failure = card::capture_failure(std::current_exception());
    return CardEntryRoute::unavailable;
  }
}

}  // namespace usb_vault::application
