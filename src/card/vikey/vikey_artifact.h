#pragma once

#include <array>
#include <cstddef>
#include <filesystem>

namespace usb_vault::card::vikey {

using Sha256Digest = std::array<std::byte, 32>;

// Proves that a local ViKey.dll matched the configured digest while holding
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

  VerifiedVikeyDll(std::filesystem::path path, void* directory_handle,
                   void* file_handle) noexcept;

  std::filesystem::path path_;
  void* directory_handle_{};
  void* file_handle_{};
};

[[nodiscard]] VerifiedVikeyDll verify_vikey_dll(
    const std::filesystem::path& deployment_directory,
    const Sha256Digest& expected_digest);

}  // namespace usb_vault::card::vikey
