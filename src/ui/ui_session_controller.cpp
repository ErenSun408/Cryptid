#include "usb_vault/ui/ui_session_controller.h"

#include "usb_vault/ui/password_ingress.h"

#include <utility>

namespace usb_vault::ui {

UiSessionController::UiSessionController(
    application::IApplicationSessionActions& session) noexcept
    : session_(session) {}

void UiSessionController::submit_password(
    const std::span<std::byte> mutable_utf8) {
  session_.authenticate_and_mount(consume_password_utf8(mutable_utf8));
}

void UiSessionController::unmount() { session_.unmount_active(); }

bool UiSessionController::mounted() const noexcept {
  return session_.mounted();
}

}  // namespace usb_vault::ui
