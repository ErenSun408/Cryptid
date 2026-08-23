#pragma once

#include "usb_vault/application/mount_orchestrator.h"

#include <string_view>

namespace usb_vault::application::detail {

class MountRuntime {
 public:
  virtual ~MountRuntime() noexcept = default;

  [[nodiscard]] virtual bool container_exists(
      std::wstring_view path) = 0;
  [[nodiscard]] virtual security::SecureBuffer derive(
      const security::SecureBuffer& password_utf8,
      const security::SecureBuffer& card_secret,
      const security::KdfV1Salt& salt) = 0;
  [[nodiscard]] virtual volume::MountResult mount(
      std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) = 0;
  [[nodiscard]] virtual volume::UnmountResult rollback_normal(
      wchar_t drive_letter) = 0;
  [[nodiscard]] virtual volume::UnmountResult rollback_force(
      wchar_t drive_letter) = 0;
  virtual void notify_mounted(wchar_t drive_letter) = 0;
};

[[nodiscard]] MountSessionResult mount_session_with_runtime(
    MountRuntime& runtime,
    const ContainerLayout& layout,
    const security::SecureBuffer& password_utf8,
    const security::SecureBuffer& card_secret);

}  // namespace usb_vault::application::detail
