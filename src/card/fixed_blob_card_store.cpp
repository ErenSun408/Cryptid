#include "usb_vault/card/fixed_blob_card_store.h"

#include "usb_vault/security/kdf_v1.h"

#include <cstddef>
#include <stdexcept>

namespace usb_vault::card {

security::SecureBuffer FixedBlobCardStore::get_blob() {
  security::SecureBuffer blob(security::kKdfV1CardSecretSize);
  for (std::size_t index = 0; index < blob.size(); ++index) {
    blob.bytes()[index] = static_cast<std::byte>(index);
  }
  return blob;
}

void FixedBlobCardStore::put_blob(const security::SecureBuffer&) {
  throw std::logic_error("fixed P1.5 card stub is read-only");
}

CardCapabilities FixedBlobCardStore::capabilities() const noexcept {
  return {};
}

}  // namespace usb_vault::card
