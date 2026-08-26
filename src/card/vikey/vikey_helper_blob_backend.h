#pragma once

#include "card/verified_local_artifact.h"
#include "card/vikey/vikey_card_store.h"

#include <chrono>
#include <filesystem>

namespace usb_vault::card::vikey {

// Owns one operation's credentials. Providers return fresh locked buffers;
// the helper backend destroys them immediately after the one-shot helper exits.
struct VikeyCredentialSet final {
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
};

class IVikeyCredentialProvider {
 public:
  virtual ~IVikeyCredentialProvider() noexcept = default;

  IVikeyCredentialProvider(const IVikeyCredentialProvider&) = delete;
  IVikeyCredentialProvider& operator=(const IVikeyCredentialProvider&) =
      delete;
  IVikeyCredentialProvider(IVikeyCredentialProvider&&) = delete;
  IVikeyCredentialProvider& operator=(IVikeyCredentialProvider&&) = delete;

  [[nodiscard]] virtual VikeyCredentialSet load_credentials() = 0;

 protected:
  IVikeyCredentialProvider() noexcept = default;
};

struct VikeyHelperBlobBackendConfig final {
  std::filesystem::path helper_path;
  Sha256Digest helper_digest{};
  std::filesystem::path vendor_directory;
  Sha256Digest vendor_digest{};
  std::chrono::milliseconds timeout{std::chrono::seconds(5)};
};

// Formal ICardStore backend over the currently available network-enabled
// development helper boundary. It preserves all artifact, process, IPC, and
// timeout checks, but must not receive production credentials or card data
// until a verifiably network-isolated production boundary replaces it.
class VikeyHelperBlobBackend final : public IVikeyBlobBackend {
 public:
  VikeyHelperBlobBackend(VikeyHelperBlobBackendConfig config,
                         IVikeyCredentialProvider& credential_provider);

  [[nodiscard]] security::SecureBuffer get_blob() override;
  void put_blob(const security::SecureBuffer& blob) override;

 private:
  VikeyHelperBlobBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
};

}  // namespace usb_vault::card::vikey
