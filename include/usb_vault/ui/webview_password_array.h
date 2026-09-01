#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Copies a supported WebView2 numeric SAFEARRAY into a page-locked owner and
// wipes every supported source element on all return and exception paths. The
// returned bytes are deliberately not normalized; the receiving product
// controller owns the single shared password-ingress policy.
[[nodiscard]] security::SecureBuffer consume_webview_password_array(
    VARIANT& password_bytes);

// Best-effort, non-throwing cleanup used when a multi-argument COM call fails
// before every argument can be consumed. Unsupported non-array values are not
// interpreted or modified.
void wipe_webview_password_array(VARIANT& password_bytes) noexcept;

}  // namespace usb_vault::ui
