#include "usb_vault/card/card_initialization_service.h"

#include "usb_vault/card/card_record_service.h"

#include <utility>

namespace usb_vault::card {

security::SecureBuffer CardInitializationService::initialize_and_authenticate(
    ICardInitializationTarget& target,
    security::SecureBuffer validated_daily_password_utf8) {
  try {
    target.ensure_provisioning_record();
  } catch (...) {
    validated_daily_password_utf8.clear();
    throw;
  }

  CardRecordService record_service(target.card_store());
  return record_service.initialize_and_authenticate(
      std::move(validated_daily_password_utf8));
}

}  // namespace usb_vault::card
