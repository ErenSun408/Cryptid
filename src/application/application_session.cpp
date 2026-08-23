#include "usb_vault/application/application_session.h"

#include "application/application_session_internal.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"

#include <stdexcept>
#include <utility>

namespace usb_vault::application {
namespace {

class ProductionApplicationSessionRuntime final
    : public detail::ApplicationSessionRuntime {
 public:
  ProductionApplicationSessionRuntime(card::ICardStore& card_store,
                                      const volume::VolumeProvider& provider)
      : card_store_(card_store), provider_(provider) {}

  [[nodiscard]] security::SecureBuffer get_card_blob() override {
    return card_store_.get_blob();
  }

  [[nodiscard]] MountSessionResult mount(
      const ContainerLayout& layout,
      const security::SecureBuffer& password_utf8,
      const security::SecureBuffer& card_secret) override {
    return mount_session(provider_, layout, password_utf8, card_secret);
  }

  [[nodiscard]] volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    return volume::unmount_with_retry(provider_, drive_letter);
  }

 private:
  card::ICardStore& card_store_;
  const volume::VolumeProvider& provider_;
};

}  // namespace

struct ApplicationSession::Impl final {
  Impl(card::ICardStore& card_store_value,
       const volume::VolumeProvider& provider_value,
       ContainerLayout layout_value)
      : card_store(card_store_value),
        provider(provider_value),
        layout(std::move(layout_value)) {}

  card::ICardStore& card_store;
  const volume::VolumeProvider& provider;
  ContainerLayout layout;
  detail::ApplicationSessionState state;
};

ApplicationSession::ApplicationSession(
    card::ICardStore& card_store,
    const volume::VolumeProvider& provider,
    ContainerLayout layout)
    : impl_(std::make_unique<Impl>(card_store, provider, std::move(layout))) {}

ApplicationSession::~ApplicationSession() noexcept = default;

void ApplicationSession::authenticate_and_mount(
    security::SecureBuffer password_utf8) {
  ProductionApplicationSessionRuntime runtime(impl_->card_store,
                                              impl_->provider);
  detail::authenticate_and_mount_with_runtime(
      runtime, impl_->layout, impl_->state, std::move(password_utf8));
}

void ApplicationSession::unmount_active() {
  ProductionApplicationSessionRuntime runtime(impl_->card_store,
                                              impl_->provider);
  detail::unmount_active_with_runtime(runtime, impl_->layout, impl_->state);
}

bool ApplicationSession::mounted() const noexcept {
  return impl_->state.mounted();
}

namespace detail {
namespace {

void clear_secrets(security::SecureBuffer& password_utf8,
                   security::SecureBuffer& card_secret) noexcept {
  password_utf8.clear();
  card_secret.clear();
}

void record_residual_mount(const ContainerLayout& layout,
                           ApplicationSessionState& state,
                           const wchar_t drive_letter) noexcept {
  if (drive_letter == layout.container_a.drive_letter) {
    state.container_a_mounted = true;
  }
  if (drive_letter == layout.container_b.drive_letter) {
    state.container_b_mounted = true;
  }
}

}  // namespace

void authenticate_and_mount_with_runtime(
    ApplicationSessionRuntime& runtime,
    const ContainerLayout& layout,
    ApplicationSessionState& state,
    security::SecureBuffer password_utf8) {
  if (state.mounted()) {
    password_utf8.clear();
    throw std::logic_error("application session is already mounted");
  }

  security::SecureBuffer card_secret;
  try {
    card_secret = runtime.get_card_blob();
    if (card_secret.size() != security::kKdfV1CardSecretSize) {
      throw std::invalid_argument(
          "card blob must contain exactly 32 bytes");
    }

    const MountSessionResult result =
        runtime.mount(layout, password_utf8, card_secret);
    state.container_a_mounted = result.container_a.has_value();
    state.container_b_mounted = result.container_b.has_value();
    clear_secrets(password_utf8, card_secret);
  } catch (const MountRollbackError& error) {
    record_residual_mount(layout, state, error.residual_drive_letter());
    clear_secrets(password_utf8, card_secret);
    throw;
  } catch (...) {
    clear_secrets(password_utf8, card_secret);
    throw;
  }
}

void unmount_active_with_runtime(ApplicationSessionRuntime& runtime,
                                 const ContainerLayout& layout,
                                 ApplicationSessionState& state) {
  // Unmount B first to minimize exposure of a potentially hidden mount. A
  // failed unmount leaves its flag set so the exact operation can be retried.
  if (state.container_b_mounted) {
    static_cast<void>(runtime.unmount(layout.container_b.drive_letter));
    state.container_b_mounted = false;
  }
  if (state.container_a_mounted) {
    static_cast<void>(runtime.unmount(layout.container_a.drive_letter));
    state.container_a_mounted = false;
  }
}

}  // namespace detail
}  // namespace usb_vault::application
