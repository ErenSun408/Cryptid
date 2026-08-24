#pragma once

#include "card/verified_local_artifact.h"

#include <filesystem>

namespace usb_vault::card::vikey {

using usb_vault::card::Sha256Digest;

// A fixed-name ViKey.dll whose bytes matched the configured digest. The token
// holds its deployment directory and file open to deny replacement, write, and
// delete. Production must place that directory below an installer-protected
// parent; this verifier cannot make a user-writable ancestor trustworthy.
// The token must remain alive until the loaded module has been unloaded.
class VerifiedVikeyDll final {
 public:
  ~VerifiedVikeyDll() noexcept;

  VerifiedVikeyDll(const VerifiedVikeyDll&) = delete;
  VerifiedVikeyDll& operator=(const VerifiedVikeyDll&) = delete;
  VerifiedVikeyDll(VerifiedVikeyDll&& other) noexcept;
  VerifiedVikeyDll& operator=(VerifiedVikeyDll&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept;

 private:
  friend VerifiedVikeyDll verify_vikey_dll(
      const std::filesystem::path& deployment_directory,
      const Sha256Digest& expected_digest);

  explicit VerifiedVikeyDll(VerifiedLocalArtifact&& artifact) noexcept;

  VerifiedLocalArtifact artifact_;
};

[[nodiscard]] VerifiedVikeyDll verify_vikey_dll(
    const std::filesystem::path& deployment_directory,
    const Sha256Digest& expected_digest);

}  // namespace usb_vault::card::vikey
