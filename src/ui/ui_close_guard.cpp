#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/diagnostics/debug_log.h"

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
    USB_VAULT_TRACE_FAILURE("ui/ui_close_guard", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return false;
  }
}

}  // namespace

bool prepare_ui_session_for_close(
    CardProductUiSessionController& controller) noexcept {
  if (controller.login_reset_completed()) return true;
  return prepare_for_close(controller);
}

bool force_ui_session_for_close(CardProductUiSessionController& controller) noexcept {
  try {
    if (controller.mounted()) controller.force_unmount_user_active();
    return !controller.mounted();
  } catch (...) { return false; }
}

}  // namespace usb_vault::ui
