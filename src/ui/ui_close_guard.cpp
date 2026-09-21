#include "usb_vault/ui/ui_close_guard.h"

namespace usb_vault::ui {
namespace {

template <typename Controller>
[[nodiscard]] bool prepare_for_close(Controller& controller) noexcept {
  if (!controller.mounted()) {
    return true;
  }
  try {
    controller.unmount();
    return !controller.mounted();
  } catch (...) {
    return false;
  }
}

}  // namespace

bool prepare_ui_session_for_close(
    CardProductUiSessionController& controller) noexcept {
  if (controller.login_reset_completed()) return true;
  return prepare_for_close(controller);
}

}  // namespace usb_vault::ui
