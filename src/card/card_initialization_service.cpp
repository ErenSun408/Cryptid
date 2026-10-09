#include "usb_vault/card/card_initialization_service.h"

#include "usb_vault/card/card_record_service.h"

#include <utility>
#include <stdexcept>

namespace usb_vault::card {

void ICardInitializationTarget::ensure_provisioning_record() {
  throw std::logic_error("legacy provisioning is unavailable");
}

ICardStore& ICardInitializationTarget::card_store() {
  throw std::logic_error("legacy card store is unavailable");
}

security::SecureBuffer ICardInitializationTarget::initialize_confirmed(
    security::ConfirmedPassword confirmed_daily_password) {
  security::SecureBuffer daily_password_utf8 =
      std::move(confirmed_daily_password).take();
  try {
    ensure_provisioning_record();
  } catch (...) {
    daily_password_utf8.clear();
    throw;
  }

  CardRecordService record_service(card_store());
  return record_service.initialize_and_authenticate(
      std::move(daily_password_utf8));
}

security::SecureBuffer CardInitializationService::initialize_and_authenticate(
    ICardInitializationTarget& target, security::ConfirmedPassword password) {
  return target.initialize_confirmed(std::move(password));
}

}  // namespace usb_vault::card
