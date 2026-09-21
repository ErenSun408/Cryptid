#pragma once
#include "usb_vault/volume/volume_provider.h"
#include <stdexcept>
namespace usb_vault::ui {
class ILoginResetMountAccess {
 public:
  virtual ~ILoginResetMountAccess() noexcept = default;
  virtual std::vector<volume::MountIdentity> mounted() = 0;
  virtual void force_unmount(const volume::MountIdentity& expected) = 0;
};
class ProviderLoginResetMountAccess final : public ILoginResetMountAccess {
 public:
  explicit ProviderLoginResetMountAccess(volume::VolumeProvider& provider) : provider_(provider) {}
  std::vector<volume::MountIdentity> mounted() override {
    std::vector<volume::MountIdentity> result;
    for (const auto& mounted : provider_.mounted_volumes())
      result.push_back(provider_.mount_identity(mounted.drive_letter));
    return result;
  }
  void force_unmount(const volume::MountIdentity& expected) override {
    if (provider_.mount_identity(expected.drive_letter) != expected)
      throw std::runtime_error("mount identity changed");
    static_cast<void>(provider_.unmount(expected.drive_letter, {.force = true}));
  }
 private:
  volume::VolumeProvider& provider_;
};
struct LoginResetPreparation final {
  std::uint32_t ticket{};
  bool mounts_known{};
  std::wstring drives;
};
}  // namespace usb_vault::ui
