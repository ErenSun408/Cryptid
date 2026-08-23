#include "usb_vault/ui/ui_close_guard.h"

namespace usb_vault::ui {

bool prepare_ui_session_for_close(UiSessionController& controller) noexcept {
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

}  // namespace usb_vault::ui
