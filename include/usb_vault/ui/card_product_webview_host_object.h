#pragma once

#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/volume/partition_catalog.h"

#include "usb_vault/volume/partition_bitlocker.h"
#include <functional>
#include <oaidl.h>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include <memory>

namespace usb_vault::ui {

// Login reset uses the same exclusive worker. Bridge status: 0 idle, 1 running,
// 2 prepared, 3 failure/unknown result, 4 force failed (skip allowed), 5 reset
// verified, 6 expired. Only a one-shot ticket and drive letters reach the page.
struct CreationWorkState final {
  std::atomic<bool> running{false};
  std::atomic<bool> interrupted{false};
  // Shares the session exclusion and deferred card-event gate with creation.
  std::atomic<bool> volume_operation{false};
  std::atomic<bool> inplace_operation{false};
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

// Injectable discovery, confirmed BitLocker preparation and fixed system-help boundary.
// The source outlives its bridge, including the bridge's background worker.
class IUiPartitionServices {
 public:
  virtual ~IUiPartitionServices() noexcept = default;
  virtual std::vector<volume::PartitionCandidate> enumerate() = 0;
  virtual std::wstring display_name(const volume::PartitionCandidate& candidate) { return volume::partition_display_name(candidate); }
  virtual bool open_disk_management() noexcept = 0;
  virtual void check(const volume::PartitionCandidate& candidate) = 0;
  virtual volume::PartitionBitLockerStatus inspect_bitlocker(const volume::PartitionCandidate&) = 0;
  virtual void decrypt_bitlocker(const volume::PartitionCandidate&, const std::function<bool()>& confirm,
                                 const std::function<bool()>& cancelled) = 0;
  virtual bool open_bitlocker_help() noexcept = 0;
};
[[nodiscard]] IUiPartitionServices& native_partition_services() noexcept;
[[nodiscard]] std::unique_ptr<IUiPartitionServices> make_partition_services(const std::filesystem::path& helper);

class IUiWindowSizeSink {
 public:
  virtual ~IUiWindowSizeSink() noexcept = default;
  // Fixed presentation presets only: standard, spacious, maximized, compact.
  virtual bool set_window_size(unsigned int preset) noexcept = 0;
  virtual unsigned int window_size() const noexcept { return 1; }
};

// How a run of shell drops is going. `state` is idle, running, done, failed
// or cancelled, in that order; the three counts are entries, not bytes, and
// `completed + failed` never exceeds `total`. Nothing here names a file: the
// page learns what arrived by listing the directory again.
struct ExternalDropStatus final {
  unsigned int state{};
  unsigned int total{};
  unsigned int completed{};
  unsigned int failed{};
};

// The verified page may name only the maintenance destination it is showing.
// Shell source paths stay in the native host and never cross this bridge, in
// either direction: what comes back is four numbers.
class IUiExternalDropTargetSink {
 public:
  virtual ~IUiExternalDropTargetSink() noexcept = default;
  virtual bool set_target(std::uint32_t id,
                          std::wstring_view relative_directory) noexcept = 0;
  virtual void clear_target() noexcept = 0;
  virtual bool request_cancel() noexcept = 0;
  virtual bool select_files() noexcept { return false; }
  virtual bool set_drop_bounds(int, int, int, int) noexcept { return false; }
  virtual bool set_drop_button_bounds(int, int, int, int) noexcept { return false; }
  virtual unsigned int file_progress() const noexcept { return 0; }
  [[nodiscard]] virtual ExternalDropStatus status() const noexcept = 0;

 protected:
  IUiExternalDropTargetSink() noexcept = default;
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
  // Deferred transition; the caller blocks further bridge mutations once
  // accepted. Only offered without authentication, mounted volumes or work.
  virtual bool request_configuration() noexcept { return false; }
  // Rebuilds an authenticated, unmounted session after retiring its bridge.
  virtual bool request_login() noexcept { return false; }

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
// Partition discovery is read-only and authenticated. beginPartitionRefresh
// starts the exclusive worker; getPartitionRefreshStatus is its only progress
// query (1 running, 2 ready, 3 failed, 4 expired; 0 no snapshot).
// getPartitionList: newline-separated "id,disk,partition,bytes,availability".
// getPartitionDisplay takes a snapshot ID and returns display-only drive letters/label.
// selectPartition re-enumerates and compares the snapshot before accepting its
// local uint32 ID, never a page path. A changed target expires the list. Refresh,
// clearPartitions, openDiskManagement and card changes retire old selections.
// beginPartitionCheck uses the selected native target on the exclusive worker;
// getPartitionCheckStatus: 0 idle, 1 running, 2 verified, 3 failed, 4 expired.
// Failure clears selection. Card changes invalidate completion; no check result
// is write authorization and no partition facts are persisted.
HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* close_sink,
    IDispatch** result,
    IUiWindowSizeSink* window_size_sink = nullptr,
    CreationWorkState* creation_work = nullptr,
    IUiExternalDropTargetSink* external_drop_sink = nullptr,
    IUiPartitionServices* partition_services = nullptr) noexcept;

}  // namespace usb_vault::ui
