#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Creates the P2-only COM automation surface for the local WebView page. It
// exposes only vendor-neutral UI state, one login input, two initialization
// inputs, authenticated A-container creation and live A display facts, and
// unified volume cleanup, and a bounded current-session operation history. No
// card identity, credential role, B-container shape, tier, full path,
// persistent history, or secret-returning member is available.
HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IDispatch** result) noexcept;

}  // namespace usb_vault::ui
