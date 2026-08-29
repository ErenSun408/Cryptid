#pragma once

#include "card/vikey/vikey_card_record_store.h"
#include "card/vikey/vikey_helper_backend.h"

#include <cstdint>

namespace usb_vault::card::vikey {

// Formal 216-byte ICardStore backend over the currently available
// network-enabled development helper. It preserves all artifact, process,
// IPC, and timeout checks, but must not receive production credentials or
// card data until a verifiably network-isolated boundary replaces it.
class VikeyHelperCardRecordBackend final
    : public IVikeyCardRecordBackend {
 public:
  VikeyHelperCardRecordBackend(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider,
      std::uint32_t expected_hardware_id);

  [[nodiscard]] security::SecureBuffer read_record() override;
  void write_record(const security::SecureBuffer& record) override;

 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
  std::uint32_t expected_hardware_id_{};
};

}  // namespace usb_vault::card::vikey
