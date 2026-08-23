#pragma once

#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/card/card_store.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include <memory>

namespace usb_vault::application {

// Owns only the volatile state needed to actively unmount drives created by the
// current process. Passwords and card material are consumed for one mount
// attempt and are never retained. The session does not classify mounted
// volumes or persist any state.
class ApplicationSession final {
 public:
  ApplicationSession(card::ICardStore& card_store,
                     const volume::VolumeProvider& provider,
                     ContainerLayout layout);
  ~ApplicationSession() noexcept;

  ApplicationSession(const ApplicationSession&) = delete;
  ApplicationSession& operator=(const ApplicationSession&) = delete;
  ApplicationSession(ApplicationSession&&) = delete;
  ApplicationSession& operator=(ApplicationSession&&) = delete;

  void authenticate_and_mount(security::SecureBuffer password_utf8);
  void unmount_active();

  [[nodiscard]] bool mounted() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
