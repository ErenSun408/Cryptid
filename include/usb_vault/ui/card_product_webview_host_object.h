#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include <oaidl.h>
#include <atomic>

namespace usb_vault::ui {

struct CreationWorkState final {
  std::atomic<bool> running{false};
  std::atomic<bool> interrupted{false};
  // Set by the page, or by the window asking to close, and cleared when the
  // next creation starts. It is a request rather than a switch: the creation
  // stops at the next point it can, which may be after the container was
  // written, and in that case the file is removed instead of published.
  std::atomic<bool> cancelled{false};
};

class IUiWindowSizeSink {
 public:
  virtual ~IUiWindowSizeSink() noexcept = default;
  // Fixed presentation presets only: standard, spacious, maximized, compact.
  virtual bool set_window_size(unsigned int preset) noexcept = 0;
};

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
    IDispatch** result,
    IUiWindowSizeSink* window_size_sink = nullptr,
    CreationWorkState* creation_work = nullptr) noexcept;

}  // namespace usb_vault::ui
