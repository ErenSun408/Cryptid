#pragma once

#include "card/verified_local_artifact.h"
#include "usb_vault/security/secure_buffer.h"

#include <chrono>
#include <filesystem>

namespace usb_vault::card::vikey {

// Owns one operation's credentials. Providers return fresh locked buffers;
// helper backends destroy them immediately after the one-shot helper exits.
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

struct VikeyHelperBackendConfig final {
  std::filesystem::path helper_path;
  Sha256Digest helper_digest{};
  std::filesystem::path vendor_directory;
  Sha256Digest vendor_digest{};
  // Probes and reads keep the short bound. A write session may scan, bootstrap,
  // commit and read back blank media in one helper run, which exceeds 5 seconds
  // on real cards; ending it early terminates the helper mid-write.
  std::chrono::milliseconds timeout{std::chrono::seconds(5)};
  std::chrono::milliseconds write_timeout{std::chrono::seconds(30)};
};

}  // namespace usb_vault::card::vikey
