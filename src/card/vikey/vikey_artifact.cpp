#include "card/vikey/vikey_artifact.h"

#include <cstdint>
#include <filesystem>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

constexpr wchar_t kDllName[] = L"ViKey.dll";
constexpr std::uint64_t kMaximumDllBytes = 16ULL * 1024ULL * 1024ULL;

}  // namespace

VerifiedVikeyDll::VerifiedVikeyDll(VerifiedLocalArtifact&& artifact) noexcept
    : artifact_(std::move(artifact)) {}

VerifiedVikeyDll::~VerifiedVikeyDll() noexcept = default;

VerifiedVikeyDll::VerifiedVikeyDll(VerifiedVikeyDll&& other) noexcept =
    default;

const std::filesystem::path& VerifiedVikeyDll::path() const noexcept {
  return artifact_.path();
}

VerifiedVikeyDll verify_vikey_dll(
    const std::filesystem::path& deployment_directory,
    const Sha256Digest& expected_digest) {
  return VerifiedVikeyDll(verify_local_artifact(
      deployment_directory / kDllName, expected_digest, kMaximumDllBytes));
}

}  // namespace usb_vault::card::vikey
