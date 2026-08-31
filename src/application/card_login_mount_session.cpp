#include "usb_vault/application/card_login_mount_session.h"

#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/volume/unmount_retry.h"

#include <stdexcept>
#include <utility>

namespace usb_vault::application {
namespace {

class ProductionCardLoginMountSessionRuntime final
    : public detail::CardLoginMountSessionRuntime {
 public:
  explicit ProductionCardLoginMountSessionRuntime(
      const volume::VolumeProvider& provider) noexcept
      : provider_(provider) {}

  [[nodiscard]] MountSessionResult mount_authenticated(
      const ContainerLayout& layout,
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& root_secret) override {
    return mount_authenticated_card_session(
        provider_, layout, login_password_utf8, root_secret);
  }

  [[nodiscard]] volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    return volume::unmount_with_retry(provider_, drive_letter);
  }

  [[nodiscard]] volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    return volume::unmount_with_forced_escalation(provider_, drive_letter);
  }

 private:
  const volume::VolumeProvider& provider_;
};

}  // namespace

struct CardLoginMountSession::Impl final {
  Impl(CardAuthenticationSession& authentication,
       const volume::VolumeProvider& provider,
       ContainerLayout layout)
      : runtime(provider),
        core(authentication, runtime, std::move(layout)) {}

  ProductionCardLoginMountSessionRuntime runtime;
  detail::CardLoginMountSessionCore core;
};

CardLoginMountSession::CardLoginMountSession(
    CardAuthenticationSession& authentication,
    const volume::VolumeProvider& provider,
    ContainerLayout layout)
    : impl_(std::make_unique<Impl>(authentication, provider,
                                   std::move(layout))) {}

CardLoginMountSession::~CardLoginMountSession() noexcept = default;

void CardLoginMountSession::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  impl_->core.consume_authenticated_login(
      std::move(root_secret), std::move(login_password_utf8));
}

void CardLoginMountSession::unmount_active() {
  impl_->core.unmount_active();
}

bool CardLoginMountSession::force_unmount_active() {
  return impl_->core.force_unmount_active();
}

bool CardLoginMountSession::mounted() const noexcept {
  return impl_->core.mounted();
}

namespace detail {
namespace {

void record_residual_mount(const ContainerLayout& layout,
                           CardLoginMountSessionState& state,
                           const wchar_t drive_letter) noexcept {
  if (drive_letter == layout.container_a.drive_letter) {
    state.container_a_mounted = true;
  }
  if (drive_letter == layout.container_b.drive_letter) {
    state.container_b_mounted = true;
  }
}

}  // namespace

CardLoginMountSessionCore::CardLoginMountSessionCore(
    CardAuthenticationSession& authentication,
    CardLoginMountSessionRuntime& runtime,
    ContainerLayout layout)
    : authentication_(authentication),
      runtime_(runtime),
      layout_(std::move(layout)) {}

void CardLoginMountSessionCore::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  if (state_.mounted() || authentication_.authenticated()) {
    root_secret.clear();
    login_password_utf8.clear();
    throw std::logic_error(
        "authenticated card mount session is already active");
  }

  try {
    authentication_.establish(std::move(root_secret));
    const MountSessionResult result = runtime_.mount_authenticated(
        layout_, login_password_utf8, authentication_.root_secret_);
    state_.container_a_mounted = result.container_a.has_value();
    state_.container_b_mounted = result.container_b.has_value();
    login_password_utf8.clear();
  } catch (const MountRollbackError& error) {
    record_residual_mount(layout_, state_, error.residual_drive_letter());
    login_password_utf8.clear();
    throw;
  } catch (...) {
    login_password_utf8.clear();
    throw;
  }
}

void CardLoginMountSessionCore::unmount_active() {
  if (state_.container_b_mounted) {
    static_cast<void>(runtime_.unmount(layout_.container_b.drive_letter));
    state_.container_b_mounted = false;
  }
  if (state_.container_a_mounted) {
    static_cast<void>(runtime_.unmount(layout_.container_a.drive_letter));
    state_.container_a_mounted = false;
  }
}

bool CardLoginMountSessionCore::force_unmount_active() {
  bool forced = false;
  if (state_.container_b_mounted) {
    forced = runtime_.unmount_forced(
        layout_.container_b.drive_letter).forced;
    state_.container_b_mounted = false;
  }
  if (state_.container_a_mounted) {
    forced = runtime_.unmount_forced(
                 layout_.container_a.drive_letter).forced ||
             forced;
    state_.container_a_mounted = false;
  }
  return forced;
}

bool CardLoginMountSessionCore::mounted() const noexcept {
  return state_.mounted();
}

}  // namespace detail
}  // namespace usb_vault::application
