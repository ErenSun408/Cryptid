#include "usb_vault/ui/ui_close_guard.h"

#include "usb_vault/security/offline_card_password_policy.h"

#include <iostream>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace {

using usb_vault::application::IApplicationSessionActions;
using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;
using usb_vault::ui::UiSessionController;
using usb_vault::ui::prepare_ui_session_for_close;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer) override { is_mounted = true; }

  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) {
      throw std::runtime_error("busy");
    }
    is_mounted = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error(
        "forced unmount is outside this contract");
  }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool is_mounted{false};
  bool reject_unmount{false};
  int unmount_calls{0};
};

class FakeProductEntry final : public ICardEntryActions {
 public:
  void initialize_and_authenticate(
      usb_vault::application::CardAuthenticationSession&,
      usb_vault::security::ConfirmedPassword) override {
    throw std::logic_error("not used by close guard");
  }
  void authenticate_login(
      usb_vault::application::ICardLoginSuccessConsumer&,
      SecureBuffer) override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] std::optional<usb_vault::application::CardEntryRoute>
  current_route() const noexcept override {
    return std::nullopt;
  }
};

class FakeProductVolume final : public IAuthenticatedCardVolumeSession {
 public:
  void create_container_a(std::uint64_t) override {
    throw std::logic_error("not used by close guard");
  }

  std::uint32_t create_container(std::wstring_view, std::uint64_t) override {
    throw std::logic_error("not used by close guard");
  }
  std::uint32_t mount_container(std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }
  void unmount_container(std::uint32_t) override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    throw std::logic_error("not used by close guard");
  }

  void consume_authenticated_login(SecureBuffer, SecureBuffer) override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] usb_vault::application::ContainerAStatus
  container_a_status() override {
    throw std::logic_error("not used by close guard");
  }
  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) throw std::runtime_error("busy");
    is_mounted = false;
  }
  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool is_mounted{};
  bool reject_unmount{};
  int unmount_calls{};
};

void test_inactive_session_closes_without_unmount() {
  FakeSession session;
  UiSessionController controller(session);
  require(prepare_ui_session_for_close(controller),
          "inactive session blocked close");
  require(session.unmount_calls == 0, "inactive session was unmounted");
}

void test_active_session_unmounts_before_close() {
  FakeSession session;
  session.is_mounted = true;
  UiSessionController controller(session);
  require(prepare_ui_session_for_close(controller),
          "successful unmount blocked close");
  require(session.unmount_calls == 1 && !session.is_mounted,
          "active session was not unmounted");
}

void test_failed_unmount_blocks_close_and_remains_retryable() {
  FakeSession session;
  session.is_mounted = true;
  session.reject_unmount = true;
  UiSessionController controller(session);
  require(!prepare_ui_session_for_close(controller),
          "failed unmount allowed close");
  require(session.unmount_calls == 1 && session.is_mounted,
          "failed unmount lost retryable state");

  session.reject_unmount = false;
  require(prepare_ui_session_for_close(controller),
          "retryable unmount did not allow close");
  require(session.unmount_calls == 2 && !session.is_mounted,
          "retry did not clear mounted state");
}

void test_product_controller_uses_the_same_close_contract() {
  FakeProductEntry entry;
  usb_vault::application::CardAuthenticationSession authentication;
  FakeProductVolume volume;
  usb_vault::security::OfflineCardPasswordPolicy policy;
  CardProductUiSessionController controller(
      entry, authentication, volume, policy);

  require(prepare_ui_session_for_close(controller),
          "inactive product session blocked close");
  volume.is_mounted = true;
  volume.reject_unmount = true;
  require(!prepare_ui_session_for_close(controller) && volume.is_mounted,
          "failed product unmount allowed close");
  volume.reject_unmount = false;
  require(prepare_ui_session_for_close(controller) && !volume.is_mounted &&
              volume.unmount_calls == 2,
          "product unmount retry did not allow close");
}

}  // namespace

int main() {
  test_inactive_session_closes_without_unmount();
  test_active_session_unmounts_before_close();
  test_failed_unmount_blocks_close_and_remains_retryable();
  test_product_controller_uses_the_same_close_contract();
  std::cout << "ui_close_guard_tests: PASS\n";
  return 0;
}
