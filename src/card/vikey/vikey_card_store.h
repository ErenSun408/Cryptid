#pragma once

#include "usb_vault/card/card_store.h"

namespace usb_vault::card::vikey {

// Private transport seam for the formal ViKey adapter. Implementations may
// use the isolated helper, but vendor credentials, paths, addresses, and error
// values must remain behind this interface.
class IVikeyBlobBackend {
 public:
  virtual ~IVikeyBlobBackend() noexcept = default;

  IVikeyBlobBackend(const IVikeyBlobBackend&) = delete;
  IVikeyBlobBackend& operator=(const IVikeyBlobBackend&) = delete;
  IVikeyBlobBackend(IVikeyBlobBackend&&) = delete;
  IVikeyBlobBackend& operator=(IVikeyBlobBackend&&) = delete;

  [[nodiscard]] virtual security::SecureBuffer get_blob() = 0;
  virtual void put_blob(const security::SecureBuffer& blob) = 0;

 protected:
  IVikeyBlobBackend() noexcept = default;
};

// Adapts the ViKey-specific backend to the application's single-blob card
// contract without retaining blob data or exposing vendor concepts upstream.
class VikeyCardStore final : public ICardStore {
 public:
  explicit VikeyCardStore(IVikeyBlobBackend& backend) noexcept;

  [[nodiscard]] security::SecureBuffer get_blob() override;
  void put_blob(const security::SecureBuffer& blob) override;
  [[nodiscard]] CardCapabilities capabilities() const noexcept override;

 private:
  IVikeyBlobBackend& backend_;
};

}  // namespace usb_vault::card::vikey
