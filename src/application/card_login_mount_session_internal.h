#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/unmount_retry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {
struct ContainerAStatus;
struct ContainerStatus;
enum class LoginMountPolicy;
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
  // Creates one self-contained outer+hidden container. The helper decides the
  // hidden size from the real allocation bitmap when zero is requested.
  virtual void create_with_hidden(
      std::wstring_view target_path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) = 0;
  virtual void publish_created(std::wstring_view staging_path,
                               std::wstring_view target_path) = 0;
  virtual void remove_incomplete(std::wstring_view staging_path) = 0;
  [[nodiscard]] virtual volume::MountResult mount_created(
      std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) = 0;
  virtual void notify_mounted(wchar_t drive_letter) = 0;
  // Returns a drive letter that the operating system currently reports as
  // free. Letters already recorded by this session are rejected by the caller.
  [[nodiscard]] virtual wchar_t allocate_drive_letter() = 0;
};

// One container adopted by the session. Nothing here is persisted: a restart
// leaves the user reselecting the container files, which is what keeps the
// 8.4 local-state layer free of a container inventory.
struct SessionContainer final {
  std::uint32_t id{};
  std::wstring path;
  std::wstring name;
  wchar_t drive_letter{};
  bool mounted{};
  // The container with a hidden volume is released before the standard ones,
  // which is the only reason the session distinguishes it. Nothing about this
  // flag crosses the application boundary.
  bool release_first{};
};

struct CardLoginMountSessionState final {
  std::vector<SessionContainer> containers;
  std::uint32_t next_container_id{1};
  // Volume password for the hidden-volume container, derived once from the
  // login input while it still existed. Keeping this instead of the login
  // password lets the user mount that container later without retyping, while
  // the retained value opens nothing else: not the card record, not a
  // standard container, and it cannot be typed at the password prompt.
  security::SecureBuffer container_b_password;
  // The password this session was opened under. Unlike the derived value
  // above it is the password itself, kept for the one operation that cannot
  // use a derivative: rewriting the card record so a second password opens it
  // too. Which of the two passwords this is, the session never asks and never
  // records, so retaining it stays symmetric under both logins.
  security::SecureBuffer login_password;

  [[nodiscard]] bool mounted() const noexcept {
    for (const SessionContainer& container : containers) {
      if (container.mounted) return true;
    }
    return false;
  }
};

// Testable core kept private to the application implementation. Friendship on
// CardAuthenticationSession grants only this class scoped access to the root.
class CardLoginMountSessionCore final {
 public:
  CardLoginMountSessionCore(
      CardAuthenticationSession& authentication,
      CardLoginMountSessionRuntime& runtime,
      ContainerLayout layout,
      ::usb_vault::application::LoginMountPolicy login_mount_policy);

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8);
  void create_container_a(std::uint64_t size_bytes);
  std::uint32_t create_container(std::wstring_view path,
                                 std::uint64_t size_bytes);
  void mount_adopted_container(std::uint32_t id);
  std::uint32_t create_container_b(
      std::uint64_t outer_size_bytes, std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8);
  [[nodiscard]] security::SecureBuffer copy_session_password();
  std::uint32_t mount_container(std::wstring_view path);
  void unmount_container(std::uint32_t id);
  [[nodiscard]] std::vector<::usb_vault::application::ContainerStatus>
  container_statuses();
  [[nodiscard]] ::usb_vault::application::ContainerAStatus
  container_a_status();
  void unmount_active();
  [[nodiscard]] bool force_unmount_active();
  [[nodiscard]] bool mounted() const noexcept;

 private:
  [[nodiscard]] security::SecureBuffer derive_standard_password();
  [[nodiscard]] wchar_t reserve_drive_letter();

  CardAuthenticationSession& authentication_;
  CardLoginMountSessionRuntime& runtime_;
  ContainerLayout layout_;
  ::usb_vault::application::LoginMountPolicy login_mount_policy_;
  CardLoginMountSessionState state_;
};

}  // namespace usb_vault::application::detail
