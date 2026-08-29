#pragma once

#include "card/vikey/vikey_helper_backend.h"
#include "card/vikey/vikey_card_store.h"

namespace usb_vault::card::vikey {

// Formal ICardStore backend over the currently available network-enabled
// development helper boundary. It preserves all artifact, process, IPC, and
// timeout checks, but must not receive production credentials or card data
// until a verifiably network-isolated production boundary replaces it.
class VikeyHelperBlobBackend final : public IVikeyBlobBackend {
 public:
  VikeyHelperBlobBackend(VikeyHelperBackendConfig config,
                         IVikeyCredentialProvider& credential_provider);

  [[nodiscard]] security::SecureBuffer get_blob() override;
  void put_blob(const security::SecureBuffer& blob) override;

 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
};

}  // namespace usb_vault::card::vikey
