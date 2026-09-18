#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include <oaidl.h>
#include <atomic>
#include <cstdint>

namespace usb_vault::ui {

struct CreationWorkState final {
  std::atomic<bool> running{false};
  std::atomic<bool> interrupted{false};
  // Shares the session exclusion and deferred card-event gate with creation.
  std::atomic<bool> volume_operation{false};
  std::atomic<std::uint64_t> card_epoch{0};
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
  virtual unsigned int window_size() const noexcept { return 1; }
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
//
// Selected batch contract (separate from login / whole-session transactions):
// beginMountBatch / beginUnmountBatch take one BSTR: 1..1024 distinct positive
// uint32 decimal IDs separated by commas, with no whitespace/leading zeroes.
// beginForceUnmountBatch uses the same selection contract for explicitly
// opted-in manual forced release. Its progress kind remains 1 (unmount).
// The complete selection is validated before any operation, then runs in the
// supplied order. Item failures continue; card changes stop subsequent items.
// getBatchProgress(): "state,kind,total,completed,currentIndex,currentId".
// State 1=running, 2=finished (inspect item outcomes), 3=worker setup failed;
// kind 0=mount/1=unmount; currentIndex is one-based, zero when none is running.
// getBatchItemId/Name/Outcome take a zero-based result index. Outcomes:
// 0=pending, 1=running, 2=succeeded, 3..10=the single-volume failure categories,
// 11=already in the requested state. Names are session file names, not paths.
// Only batch queries are available during batch execution. Card changes or
// authentication loss deny old snapshots; another mutation discards them.
// No batch outcome grants single-volume force permission or manual cancellation.
HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* close_sink,
    IDispatch** result,
    IUiWindowSizeSink* window_size_sink = nullptr,
    CreationWorkState* creation_work = nullptr) noexcept;

}  // namespace usb_vault::ui
