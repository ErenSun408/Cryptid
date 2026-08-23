#include "usb_vault/ui/ui_close_guard.h"

#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;
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

  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool is_mounted{false};
  bool reject_unmount{false};
  int unmount_calls{0};
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

}  // namespace

int main() {
  test_inactive_session_closes_without_unmount();
  test_active_session_unmounts_before_close();
  test_failed_unmount_blocks_close_and_remains_retryable();
  std::cout << "ui_close_guard_tests: PASS\n";
  return 0;
}
