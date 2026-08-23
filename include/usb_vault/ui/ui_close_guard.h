#pragma once

#include "usb_vault/ui/ui_session_controller.h"

namespace usb_vault::ui {

// Returns true only when the window may be destroyed without abandoning an
// application-owned mount. Failures remain retryable through the session state
// and never escape a Win32 window procedure.
[[nodiscard]] bool prepare_ui_session_for_close(
    UiSessionController& controller) noexcept;

}  // namespace usb_vault::ui
