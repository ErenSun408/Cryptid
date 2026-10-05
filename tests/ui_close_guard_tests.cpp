#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/ui/exit_hotkey.h"

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
  void forget_container(std::uint32_t) override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] bool remembering_container_paths() override { return true; }
  void set_remembering_container_paths(bool) override {
    throw std::logic_error("not used by close guard");
  }
  std::uint32_t adopt_container(std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }
  [[nodiscard]] bool mounting_imported_containers() override { return false; }
  void set_mounting_imported_containers(bool) override {
    throw std::logic_error("not used by close guard");
  }
  // The file manager is not part of this fixture. P3.5 put these on the
  // product boundary and P3.6 is where the controller starts using them; until
  // then a call here is a mistake rather than a case worth modelling.
  [[nodiscard]] usb_vault::volume::VolumeDirectoryListing
  list_container_directory(std::uint32_t, std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void delete_container_entry(std::uint32_t, std::wstring_view,
                              std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void rename_container_entry(std::uint32_t, std::wstring_view,
                              std::wstring_view, std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void create_container_directory(std::uint32_t, std::wstring_view,
                                  std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void move_container_entry(std::uint32_t, std::wstring_view, std::wstring_view,
                            std::uint32_t, std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void copy_container_entry(std::uint32_t, std::wstring_view, std::wstring_view,
                            std::uint32_t, std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void export_container_entry(std::uint32_t, std::wstring_view,
                              std::wstring_view,
                              std::wstring_view) override {
    throw std::logic_error("not used by close guard");
  }

  void export_container_file(std::uint32_t, std::wstring_view) override {
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
    if (!retain_mount) is_mounted = false;
  }
  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("not used by close guard");
  }
  void force_unmount_user_active() override {
    ++force_calls;
    if (reject_force) throw std::runtime_error("force failed");
    if (!retain_mount) is_mounted = false;
  }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool is_mounted{};
  bool reject_unmount{};
  int unmount_calls{};
  int force_calls{};
  bool reject_force{}, retain_mount{};
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
  require(!prepare_ui_session_for_close(controller) && volume.is_mounted && volume.force_calls == 0,
          "failed product unmount allowed close");
  volume.reject_unmount = false;
  volume.retain_mount = true;
  require(!prepare_ui_session_for_close(controller) && volume.is_mounted && volume.force_calls == 0,
          "residual mount allowed normal close or forced cleanup");
  volume.retain_mount = false;
  require(prepare_ui_session_for_close(controller) && !volume.is_mounted &&
              volume.unmount_calls == 3 && volume.force_calls == 0,
          "product unmount retry did not allow close");
}

void test_forced_close_preserves_failed_mounts() {
  FakeProductEntry entry;
  usb_vault::application::CardAuthenticationSession authentication;
  FakeProductVolume volume;
  usb_vault::security::OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller(entry, authentication, volume, policy, chooser);
  using usb_vault::ui::force_ui_session_for_close;
  require(force_ui_session_for_close(controller) && volume.force_calls == 0, "empty session forced");
  volume.is_mounted = true; volume.reject_force = true;
  require(!force_ui_session_for_close(controller) && volume.is_mounted, "failure allowed close");
  volume.reject_force = false; volume.retain_mount = true;
  require(!force_ui_session_for_close(controller), "residual mount allowed close");
  volume.retain_mount = false;
  require(force_ui_session_for_close(controller) && !volume.is_mounted && volume.unmount_calls == 0, "force route not used");
}

void test_exit_hotkey_counter() {
  using usb_vault::ui::ExitHotkeyCounter;
  ExitHotkeyCounter c({true, 123, 3});
  auto press = [&](std::uint64_t time) { const bool fired = c.input(123, true, false, time); c.input(123, false, false, time); return fired; };
  require(!press(0) && !press(500) && press(1000), "one-second boundary failed");
  require(!press(2000) && !press(2600) && !press(3001) && press(3100), "rolling window failed");
  require(!c.input(123,true,false,4000) && !c.input(123,true,false,4100) && !c.input(123,true,false,4200), "hold counted repeats");
  c.input(123,false,false,4201);
  require(!press(4300) && press(4400), "release did not rearm");
  require(!press(5000), "new sequence fired"); c.input(65,true,false,5001); c.input(65,false,false,5002);
  require(!press(5100) && !press(5200) && press(5300), "other key did not reset");
  c.input(0xA2,true,false,6000);
  require(!press(6100) && !press(6200) && !press(6300), "modified key fired");
  c.input(0xA2,false,false,6400);
  require(!c.input(123,true,true,6500) && !c.input(123,true,true,6510), "injected key fired");
  require(!press(6600) && !press(6700) && press(6800), "injected input altered state");
  ExitHotkeyCounter captured({true, 65, 1});
  require(!captured.input(65,true,false,0,true), "capture fired exit");
  require(!captured.input(65,true,false,1), "capture release leaked repeat");
  captured.input(65,false,false,2);
  require(captured.input(65,true,false,3), "capture did not resume");
  ExitHotkeyCounter arrow({true, 37, 1});
  require(arrow.input(37,true,false,0), "arrow not supported");
  ExitHotkeyCounter off({false, 123, 1}); require(!off.input(123,true,false,0), "disabled key fired");
  ExitHotkeyCounter one({true, 65, 1}); one.held(65);
  require(!one.input(65,true,false,0), "held key fired after enabling");
  one.input(65,false,false,1); require(one.input(65,true,false,2), "single press failed");
}

}  // namespace

int main() {
  test_exit_hotkey_counter();
  test_forced_close_preserves_failed_mounts();
  test_product_controller_uses_the_same_close_contract();
  std::cout << "ui_close_guard_tests: PASS\n";
  return 0;
}
