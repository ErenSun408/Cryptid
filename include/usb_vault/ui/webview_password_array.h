#pragma once

#include "usb_vault/ui/ui_session_controller.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Consumes either a one-dimensional VT_UI1 SAFEARRAY or WebView2 Runtime's
// one-dimensional VT_VARIANT SAFEARRAY of byte-range VT_I4 values. The array
// allocation remains caller-owned; accepted byte values are wiped on every
// submit path before forwarding through UiSessionController.
void submit_webview_password_array(VARIANT& password_bytes,
                                   UiSessionController& controller);

}  // namespace usb_vault::ui
