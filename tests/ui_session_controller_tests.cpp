#include "usb_vault/ui/ui_session_controller.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::UiSessionController;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer password_utf8) override {
    ++submit_calls;
    require(password_utf8.size() == expected.size(),
            "controller forwarded a different password size");
    for (std::size_t index = 0; index < expected.size(); ++index) {
      require(password_utf8.bytes()[index] == expected[index],
              "controller forwarded different password bytes");
    }
    password_utf8.clear();
    if (reject_submit) {
      throw std::runtime_error("mount rejected");
    }
    is_mounted = true;
  }

  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) {
      throw std::runtime_error("unmount rejected");
    }
    is_mounted = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error(
        "forced unmount is outside this contract");
  }
  [[nodiscard]] bool mounted() const noexcept override {
    return is_mounted;
  }

  std::array<std::byte, 4> expected{
      std::byte{'p'}, std::byte{0x00}, std::byte{0xC3}, std::byte{0xA4}};
  bool is_mounted{false};
  bool reject_submit{false};
  bool reject_unmount{false};
  int submit_calls{0};
  int unmount_calls{0};
};

void require_wiped(const std::array<std::byte, 4>& input) {
  for (const std::byte value : input) {
    require(value == std::byte{0}, "UI input was not wiped");
  }
}

void test_submit_consumes_input_and_exposes_unified_state() {
  FakeSession session;
  UiSessionController controller(session);
  auto input = session.expected;

  controller.submit_password(input);

  require_wiped(input);
  require(session.submit_calls == 1, "submit was not forwarded once");
  require(controller.mounted(), "mounted state was not forwarded");

  controller.unmount();
  require(session.unmount_calls == 1, "unmount was not forwarded once");
  require(!controller.mounted(), "unmounted state was not forwarded");
}

void test_submit_failure_still_wipes_input() {
  FakeSession session;
  session.reject_submit = true;
  UiSessionController controller(session);
  auto input = session.expected;

  try {
    controller.submit_password(input);
  } catch (const std::runtime_error&) {
    require_wiped(input);
    require(!controller.mounted(), "failed submit reported mounted");
    return;
  }
  throw std::runtime_error("submit failure was not propagated");
}

void test_unmount_failure_preserves_session_state() {
  FakeSession session;
  session.is_mounted = true;
  session.reject_unmount = true;
  UiSessionController controller(session);

  try {
    controller.unmount();
  } catch (const std::runtime_error&) {
    require(controller.mounted(), "failed unmount hid active state");
    return;
  }
  throw std::runtime_error("unmount failure was not propagated");
}

}  // namespace

int main() {
  test_submit_consumes_input_and_exposes_unified_state();
  test_submit_failure_still_wipes_input();
  test_unmount_failure_preserves_session_state();

  std::cout << "ui_session_controller_tests: PASS\n";
  return 0;
}
