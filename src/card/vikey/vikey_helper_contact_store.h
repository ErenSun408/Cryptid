#pragma once

#include "card/vikey/vikey_helper_backend.h"
#include "usb_vault/contacts/contact_store_v1.h"

#include <cstdint>

namespace usb_vault::card::vikey {

// Contact list storage over the development helper (P5.14). Each read and
// write is one hardware-bound helper session on the card identified by
// expected_hardware_id. Only the whole region can be read and only a whole
// copy written; any other range is rejected before the helper starts. A user
// area too small for the region surfaces as ContactStoreCapacityError. Same
// development-boundary restrictions as VikeyHelperCardRecordBackend.
class VikeyHelperContactStoreStorage final
    : public contacts::IContactStoreStorage {
 public:
  VikeyHelperContactStoreStorage(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider,
      std::uint32_t expected_hardware_id);

  // The helper checks the real capacity on every operation.
  [[nodiscard]] std::size_t capacity() override {
    return contacts::kContactStoreV1RegionEnd;
  }
  [[nodiscard]] security::SecureBuffer read(std::size_t offset,
                                            std::size_t length) override;
  void write(std::size_t offset, std::span<const std::byte> bytes) override;

 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
  std::uint32_t expected_hardware_id_{};
};

}  // namespace usb_vault::card::vikey
