#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/volume/unmount_retry.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace usb_vault::application {
struct ContainerAStatus;
}

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
  [[nodiscard]] virtual bool container_exists(std::wstring_view path) = 0;
  [[nodiscard]] virtual std::optional<std::uint64_t>
  container_size_if_present(std::wstring_view path) = 0;
  [[nodiscard]] virtual security::SecureBuffer derive_container_password(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& root_secret,
      const security::KdfV1Salt& salt) = 0;
  virtual void create_standard(
      std::wstring_view target_path, std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) = 0;
  virtual void publish_created(std::wstring_view staging_path,
                               std::wstring_view target_path) = 0;
  virtual void remove_incomplete(std::wstring_view staging_path) = 0;
  [[nodiscard]] virtual volume::MountResult mount_created(
      std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) = 0;
  virtual void notify_mounted(wchar_t drive_letter) = 0;
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
  void create_container_a(std::uint64_t size_bytes);
  [[nodiscard]] ::usb_vault::application::ContainerAStatus
  container_a_status();
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
