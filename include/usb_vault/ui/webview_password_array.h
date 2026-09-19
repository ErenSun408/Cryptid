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

// Clears every string argument of one marshalled call. A page is expected to
// send secrets as byte arrays, but a value that arrives as a string was still
// marshalled into this process, and the caller frees it without clearing it.
// Anything that is not a string was never read as one and is left exactly as
// it arrived.
void wipe_dispatch_text_arguments(DISPPARAMS* parameters) noexcept;

// Scope guard for the above, so a bridge covers every return path of Invoke
// by declaring one object.
class DispatchTextWiper final {
 public:
  explicit DispatchTextWiper(DISPPARAMS* const parameters) noexcept
      : parameters_(parameters) {}

  ~DispatchTextWiper() noexcept { wipe_dispatch_text_arguments(parameters_); }

  DispatchTextWiper(const DispatchTextWiper&) = delete;
  DispatchTextWiper& operator=(const DispatchTextWiper&) = delete;
  DispatchTextWiper(DispatchTextWiper&&) = delete;
  DispatchTextWiper& operator=(DispatchTextWiper&&) = delete;

 private:
  DISPPARAMS* parameters_;
};

}  // namespace usb_vault::ui
