#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/volume/unmount_retry.h"

namespace usb_vault::application::detail {

class CardLoginMountSessionRuntime {
 public:
  virtual ~CardLoginMountSessionRuntime() noexcept = default;

  [[nodiscard]] virtual MountSessionResult mount_authenticated(
      const ContainerLayout& layout,
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& root_secret) = 0;
  [[nodiscard]] virtual volume::UnmountResult unmount(
      wchar_t drive_letter) = 0;
  [[nodiscard]] virtual volume::UnmountEscalationResult unmount_forced(
      wchar_t drive_letter) = 0;
};

struct CardLoginMountSessionState final {
  bool container_a_mounted{};
  bool container_b_mounted{};

  [[nodiscard]] bool mounted() const noexcept {
    return container_a_mounted || container_b_mounted;
  }
};

// Testable core kept private to the application implementation. Friendship on
// CardAuthenticationSession grants only this class scoped access to the root.
class CardLoginMountSessionCore final {
 public:
  CardLoginMountSessionCore(
      CardAuthenticationSession& authentication,
      CardLoginMountSessionRuntime& runtime,
      ContainerLayout layout);

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8);
  void unmount_active();
  [[nodiscard]] bool force_unmount_active();
  [[nodiscard]] bool mounted() const noexcept;

 private:
  CardAuthenticationSession& authentication_;
  CardLoginMountSessionRuntime& runtime_;
  ContainerLayout layout_;
  CardLoginMountSessionState state_;
};

}  // namespace usb_vault::application::detail
