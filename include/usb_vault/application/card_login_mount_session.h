#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace usb_vault::application {

class CardAuthenticationSession;

// Whether a successful login also mounts the containers that already exist.
// The product default is `manual`: login establishes authentication only, and
// every mount is a separate, explicitly requested action. `mount_on_login` is
// opt-in and reinstates the P2.14 behaviour of mounting during login.
enum class LoginMountPolicy {
  manual,
  mount_on_login,
};

struct ContainerAStatus final {
  bool exists{};
  bool mounted{};
  std::uint64_t size_bytes{};
  wchar_t drive_letter{};
};

class ContainerCreationCleanupError final : public std::runtime_error {
 public:
  ContainerCreationCleanupError(std::exception_ptr operation_failure,
                                std::exception_ptr cleanup_failure);

  [[nodiscard]] const std::exception_ptr& operation_failure() const noexcept;
  [[nodiscard]] const std::exception_ptr& cleanup_failure() const noexcept;

 private:
  std::exception_ptr operation_failure_;
  std::exception_ptr cleanup_failure_;
};

// Combined product boundary required by the native UI: a successful login
// handoff and the lifecycle of any resulting active volumes must be the same
// session object. It still exposes no P1 raw-card authentication entry point.
class IAuthenticatedCardVolumeSession : public ICardLoginSuccessConsumer,
                                        public IActiveVolumeSession {
 public:
  ~IAuthenticatedCardVolumeSession() noexcept override = default;

  IAuthenticatedCardVolumeSession(
      const IAuthenticatedCardVolumeSession&) = delete;
  IAuthenticatedCardVolumeSession& operator=(
      const IAuthenticatedCardVolumeSession&) = delete;
  IAuthenticatedCardVolumeSession(
      IAuthenticatedCardVolumeSession&&) = delete;
  IAuthenticatedCardVolumeSession& operator=(
      IAuthenticatedCardVolumeSession&&) = delete;

  // Creates the independent standard A container for the authenticated card,
  // publishes it without replacement, and mounts it before returning. Root
  // bytes and derived passwords never cross this boundary.
  virtual void create_container_a(std::uint64_t size_bytes) = 0;

  // Returns only the live, non-secret A-container facts required by the
  // authenticated product UI. No path, password, root, tier, B-container
  // shape, or historical state crosses this boundary.
  [[nodiscard]] virtual ContainerAStatus container_a_status() = 0;

 protected:
  IAuthenticatedCardVolumeSession() noexcept = default;
};

// P2 Card Record login success handler. It establishes volatile card
// authentication before attempting any post-authentication volume work, never
// reads the card again, and retains only the root plus unified active-volume
// state. The original login input exists only for the synchronous mount call.
class CardLoginMountSession final : public IAuthenticatedCardVolumeSession {
 public:
  CardLoginMountSession(CardAuthenticationSession& authentication,
                        const volume::VolumeProvider& provider,
                        ContainerLayout layout,
                        std::filesystem::path volume_creator_helper_path,
                        LoginMountPolicy login_mount_policy);
  ~CardLoginMountSession() noexcept override;

  CardLoginMountSession(const CardLoginMountSession&) = delete;
  CardLoginMountSession& operator=(const CardLoginMountSession&) = delete;
  CardLoginMountSession(CardLoginMountSession&&) = delete;
  CardLoginMountSession& operator=(CardLoginMountSession&&) = delete;

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8) override;
  void create_container_a(std::uint64_t size_bytes) override;
  [[nodiscard]] ContainerAStatus container_a_status() override;

  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;
  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
