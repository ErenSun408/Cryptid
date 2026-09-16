#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include <oaidl.h>
#include <atomic>

namespace usb_vault::ui {

struct CreationWorkState final {
  std::atomic<bool> running{false};
  std::atomic<bool> interrupted{false};
  // Shares the session exclusion and deferred card-event gate with creation.
  std::atomic<bool> volume_operation{false};
  void begin() noexcept { phase_.store(Phase::cancellable); }
  void finish() noexcept { phase_.store(Phase::idle); }
  [[nodiscard]] bool cancellation_requested() const noexcept {
    return phase_.load() == Phase::cancel_requested;
  }
  [[nodiscard]] bool can_cancel() const noexcept {
    return phase_.load() == Phase::cancellable;
  }
  // Shared by the page and window. A request and the publication boundary
  // compete on one atomic value, so an accepted request cannot be published.
  [[nodiscard]] bool request_cancel() noexcept {
    auto expected = Phase::cancellable;
    return phase_.compare_exchange_strong(expected, Phase::cancel_requested) ||
           expected == Phase::cancel_requested;
  }
  [[nodiscard]] bool begin_finalization() noexcept {
    auto expected = Phase::cancellable;
    return phase_.compare_exchange_strong(expected, Phase::finalizing);
  }

 private:
  enum class Phase { idle, cancellable, cancel_requested, finalizing };
  std::atomic<Phase> phase_{Phase::idle};
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
