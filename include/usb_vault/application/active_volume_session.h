#pragma once

namespace usb_vault::application {

// Minimal process-local lifecycle for volumes mounted by this process. It
// exposes no authentication entry point, secret, volume shape, or mode.
class IActiveVolumeSession {
 public:
  virtual ~IActiveVolumeSession() noexcept = default;

  IActiveVolumeSession(const IActiveVolumeSession&) = delete;
  IActiveVolumeSession& operator=(const IActiveVolumeSession&) = delete;
  IActiveVolumeSession(IActiveVolumeSession&&) = delete;
  IActiveVolumeSession& operator=(IActiveVolumeSession&&) = delete;

  virtual void unmount_active() = 0;
  // Emergency cleanup for physical-card removal. Returns true when an
  // ordinary unmount was rejected and force was required.
  [[nodiscard]] virtual bool force_unmount_active() = 0;
  [[nodiscard]] virtual bool mounted() const noexcept = 0;

 protected:
  IActiveVolumeSession() noexcept = default;
};

}  // namespace usb_vault::application
