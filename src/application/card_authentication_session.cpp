#include "usb_vault/application/card_authentication_session.h"

#include "usb_vault/card/card_record_v1.h"

#include <stdexcept>
#include <utility>

namespace usb_vault::application {

void CardAuthenticationSession::establish(
    security::SecureBuffer root_secret) {
  if (root_secret.size() != card::kCardRecordV1RootSecretSize) {
    root_secret.clear();
    throw std::invalid_argument(
        "authenticated card root secret has the wrong size");
  }
  if (authenticated()) {
    root_secret.clear();
    throw std::logic_error(
        "card authentication session is already active");
  }
  root_secret_ = std::move(root_secret);
}

void CardAuthenticationSession::clear_authentication() noexcept {
  root_secret_ = security::SecureBuffer{};
}

bool CardAuthenticationSession::authenticated() const noexcept {
  return root_secret_.size() == card::kCardRecordV1RootSecretSize;
}

}  // namespace usb_vault::application
