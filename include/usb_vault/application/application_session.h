#pragma once

#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/card/card_store.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include <memory>

namespace usb_vault::application {

class IApplicationSessionActions {
 public:
  virtual ~IApplicationSessionActions() noexcept = default;

  virtual void authenticate_and_mount(
      security::SecureBuffer password_utf8) = 0;
  virtual void unmount_active() = 0;
  // Unmounts the active session even when a drive is busy, for the single
  // case where the key material behind it can no longer be trusted. Returns
  // true when a forced unmount was required, which can discard data the
  // filesystem has not written yet.
  [[nodiscard]] virtual bool force_unmount_active() = 0;
  [[nodiscard]] virtual bool mounted() const noexcept = 0;
};

// Owns only the volatile state needed to actively unmount drives created by the
// current process. Passwords and card material are consumed for one mount
// attempt and are never retained. The session does not classify mounted
// volumes or persist any state.
class ApplicationSession final : public IApplicationSessionActions {
 public:
  ApplicationSession(card::ICardStore& card_store,
                     const volume::VolumeProvider& provider,
                     ContainerLayout layout);
  ~ApplicationSession() noexcept override;

  ApplicationSession(const ApplicationSession&) = delete;
  ApplicationSession& operator=(const ApplicationSession&) = delete;
  ApplicationSession(ApplicationSession&&) = delete;
  ApplicationSession& operator=(ApplicationSession&&) = delete;

  void authenticate_and_mount(
      security::SecureBuffer password_utf8) override;
  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;

  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
