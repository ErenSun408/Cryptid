#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace usb_vault::card {

using Sha256Digest = std::array<std::byte, 32>;

// A fixed-drive file whose bytes matched an expected digest. The token keeps
// both the file and its containing directory open without write/delete sharing
// so the verified path cannot be replaced before the consumer finishes.
class VerifiedLocalArtifact final {
 public:
  ~VerifiedLocalArtifact() noexcept;

  VerifiedLocalArtifact(const VerifiedLocalArtifact&) = delete;
  VerifiedLocalArtifact& operator=(const VerifiedLocalArtifact&) = delete;
  VerifiedLocalArtifact(VerifiedLocalArtifact&& other) noexcept;
  VerifiedLocalArtifact& operator=(VerifiedLocalArtifact&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  // Only removal-enabled verification grants DELETE on the pinned file.
  [[nodiscard]] bool remove_verified_file() noexcept;

 private:
  friend VerifiedLocalArtifact verify_local_artifact(
      const std::filesystem::path& path,
      const Sha256Digest& expected_digest,
      std::uint64_t maximum_bytes, bool allow_delete);

  VerifiedLocalArtifact(std::filesystem::path path, void* directory_handle,
                        void* file_handle) noexcept;

  std::filesystem::path path_;
  void* directory_handle_{};
  void* file_handle_{};
};

[[nodiscard]] VerifiedLocalArtifact verify_local_artifact(
    const std::filesystem::path& path,
    const Sha256Digest& expected_digest,
    std::uint64_t maximum_bytes, bool allow_delete = false);

}  // namespace usb_vault::card
