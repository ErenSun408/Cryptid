#pragma once

#include "card/vikey/vikey_helper_backend.h"
#include "card/vikey/vikey_helper_client.h"

namespace usb_vault::card::vikey {

// ViKey-private enrollment boundary for the network-enabled development
// helper. It stores only non-secret artifact configuration and borrows the
// credential provider; every probe/bootstrap operation obtains a fresh
// locked credential set and destroys it after the one-shot helper exits.
class VikeyHelperCardEnrollment final {
 public:
  VikeyHelperCardEnrollment(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider);

  [[nodiscard]] VikeyCardMediaProbe probe();
  void bootstrap(const VikeyCardMediaProbe& context);

 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
};

}  // namespace usb_vault::card::vikey
