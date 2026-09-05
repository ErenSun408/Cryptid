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

using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;
using usb_vault::ui::prepare_ui_session_for_close;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

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
  void set_second_card_password(SecureBuffer, SecureBuffer) override {
    throw std::logic_error("not used by close guard");
  }
  void reset_card_record(
      const usb_vault::application::IVolatileCardAuthenticationSession&)
      override {
    throw std::logic_error("card reset is outside the close contract");
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
  std::uint32_t create_container_b(
      std::wstring_view, std::uint64_t, std::uint64_t,
      const usb_vault::security::SecureBuffer&) override {
    throw std::logic_error("container B is outside the close contract");
  }

  usb_vault::security::SecureBuffer copy_session_password() override {
    throw std::logic_error("container B is outside the close contract");
  }

  std::uint32_t mount_container(std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }
  void mount_adopted_container(std::uint32_t) override {
    throw std::logic_error("adopted mounts are not part of this fake");
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
  [[nodiscard]] std::vector<usb_vault::application::OperationEvent>
  operation_history() override {
    throw std::logic_error("not used by close guard");
  }
  void release_session_secrets() noexcept override {}

  void clear_operation_history() noexcept override {}
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

// Closing never locates a container file, so the chooser only has to exist.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  [[nodiscard]] std::wstring choose_container_directory() override {
    return {};
  }
  [[nodiscard]] std::wstring choose_existing_container() override {
    return {};
  }
};

void test_product_controller_uses_the_same_close_contract() {
  FakeProductEntry entry;
  usb_vault::application::CardAuthenticationSession authentication;
  FakeProductVolume volume;
  usb_vault::security::OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller(
      entry, authentication, volume, policy, chooser);

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
  test_product_controller_uses_the_same_close_contract();
  std::cout << "ui_close_guard_tests: PASS\n";
  return 0;
}
