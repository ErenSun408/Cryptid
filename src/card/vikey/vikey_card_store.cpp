#include "card/vikey/vikey_card_store.h"

#include "card/vikey/vikey_session.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/security/kdf_v1.h"

#include <system_error>

namespace usb_vault::card::vikey {

static_assert(kVikeyBlobBytes == security::kKdfV1CardSecretSize);

VikeyCardStore::VikeyCardStore(IVikeyBlobBackend& backend) noexcept
    : backend_(backend) {}

security::SecureBuffer VikeyCardStore::get_blob() {
  security::SecureBuffer blob = backend_.get_blob();
  if (blob.size() != kVikeyBlobBytes) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey backend returned an invalid blob size");
  }
  return blob;
}

void VikeyCardStore::put_blob(const security::SecureBuffer& blob) {
  if (blob.size() != kVikeyBlobBytes) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey blob must contain exactly 32 bytes");
  }
  backend_.put_blob(blob);
}

CardCapabilities VikeyCardStore::capabilities() const noexcept { return {}; }

}  // namespace usb_vault::card::vikey
