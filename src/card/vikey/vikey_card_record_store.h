#pragma once

#include "usb_vault/card/card_store.h"

namespace usb_vault::card::vikey {

// Private transport seam for the formal 216-byte ViKey card-record adapter.
// Vendor credentials, paths, physical journal offsets, and error values remain
// behind this interface.
class IVikeyCardRecordBackend {
 public:
  virtual ~IVikeyCardRecordBackend() noexcept = default;

  IVikeyCardRecordBackend(const IVikeyCardRecordBackend&) = delete;
  IVikeyCardRecordBackend& operator=(const IVikeyCardRecordBackend&) = delete;
  IVikeyCardRecordBackend(IVikeyCardRecordBackend&&) = delete;
  IVikeyCardRecordBackend& operator=(IVikeyCardRecordBackend&&) = delete;

  [[nodiscard]] virtual security::SecureBuffer read_record() = 0;
  virtual void write_record(const security::SecureBuffer& record) = 0;

 protected:
  IVikeyCardRecordBackend() noexcept = default;
};

// Adapts the ViKey-specific journal backend to the application's unchanged
// single-blob contract while enforcing the P2 fixed logical record size.
class VikeyCardRecordStore final : public ICardStore {
 public:
  explicit VikeyCardRecordStore(IVikeyCardRecordBackend& backend) noexcept;

  [[nodiscard]] security::SecureBuffer get_blob() override;
  void put_blob(const security::SecureBuffer& blob) override;
  [[nodiscard]] CardCapabilities capabilities() const noexcept override;

 private:
  IVikeyCardRecordBackend& backend_;
};

}  // namespace usb_vault::card::vikey
