#pragma once

#include "usb_vault/ui/ui_session_controller.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Creates the narrow COM automation surface exposed to the local WebView2
// page. The returned interface owns one reference and exposes only password
// submission; it does not expose application or volume classification state.
HRESULT create_webview_host_object(UiSessionController& controller,
                                   IDispatch** result) noexcept;

}  // namespace usb_vault::ui
