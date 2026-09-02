#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include <oaidl.h>

namespace usb_vault::ui {

// Asks the host window to close. A card returned to the provisioning marker
// cannot be re-routed inside the running process: the insertion's route is
// fixed for its lifetime, so a fresh start is the only place the new state is
// classified. Implementations must not throw.
class IUiCloseRequestSink {
 public:
  virtual ~IUiCloseRequestSink() noexcept = default;

  IUiCloseRequestSink(const IUiCloseRequestSink&) = delete;
  IUiCloseRequestSink& operator=(const IUiCloseRequestSink&) = delete;
  IUiCloseRequestSink(IUiCloseRequestSink&&) = delete;
  IUiCloseRequestSink& operator=(IUiCloseRequestSink&&) = delete;

  virtual void request_close() noexcept = 0;

 protected:
  IUiCloseRequestSink() noexcept = default;
};

// Creates the P2-only COM automation surface for the local WebView page. It
// exposes only vendor-neutral UI state, one login input, two initialization
// inputs, authenticated A-container creation and live A display facts, and
// unified volume cleanup, and a bounded current-session operation history. No
// card identity, credential role, B-container shape, tier, full path,
// persistent history, or secret-returning member is available.
HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* close_sink,
    IDispatch** result) noexcept;

}  // namespace usb_vault::ui
