#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"

#include <memory>

namespace usb_vault::application {

class CardAuthenticationSession;

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
                        ContainerLayout layout);
  ~CardLoginMountSession() noexcept override;

  CardLoginMountSession(const CardLoginMountSession&) = delete;
  CardLoginMountSession& operator=(const CardLoginMountSession&) = delete;
  CardLoginMountSession(CardLoginMountSession&&) = delete;
  CardLoginMountSession& operator=(CardLoginMountSession&&) = delete;

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8) override;

  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;
  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
