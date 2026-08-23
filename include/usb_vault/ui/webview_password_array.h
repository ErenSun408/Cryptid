#pragma once

#include "usb_vault/ui/ui_session_controller.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Consumes a one-dimensional VT_UI1 SAFEARRAY supplied by the WebView2 COM
// bridge. The array allocation remains owned by the caller, but its byte
// contents are wiped by UiSessionController on every submit path.
void submit_webview_password_array(VARIANT& password_bytes,
                                   UiSessionController& controller);

}  // namespace usb_vault::ui
