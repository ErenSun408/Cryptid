#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

namespace usb_vault::ui {

// Returns true only when the window may be destroyed without abandoning an
// application-owned mount. Authentication without mounted volumes may close;
// any active or residual volume must be released successfully first. Failures
// remain retryable through the session state and never escape a Win32 window
// procedure.
[[nodiscard]] bool prepare_ui_session_for_close(
    CardProductUiSessionController& controller) noexcept;
// Explicit opt-in shortcut only; the caller owns session exclusion.
[[nodiscard]] bool force_ui_session_for_close(
    CardProductUiSessionController& controller) noexcept;

}  // namespace usb_vault::ui
