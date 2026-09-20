#pragma once

#include "usb_vault/ui/card_product_webview_host_object.h"

#include <Windows.h>
#include <oleidl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace usb_vault::ui {

// Copies one entry the shell handed over into the destination the page named.
// It throws to refuse, exactly as the session layer it stands for does, and it
// is asked repeatedly through `cancelled` while a large entry is in flight.
using ExternalImportStep = std::function<void(
    std::uint32_t id, std::wstring_view relative_directory,
    std::wstring_view absolute_source, const std::function<bool()>& cancelled)>;

// The paths a shell data object carries, or nothing.
//
// 🔴 `CF_HDROP` is the only format this product accepts, and the refusal of
// everything else is a deniability decision rather than a simplification. The
// other ways a drag can carry content -- a virtual file from a mail client or
// from inside a zip, say -- are rendered on demand, which means writing the
// content to an ordinary temporary file on an unencrypted disk first and only
// then copying it into the volume. That file is exactly the kind of trace the
// product exists to avoid, and nothing here can promise to remove it. A
// `CF_HDROP` names files that are already on disk, so accepting one adds
// nothing that was not there before the drag started.
[[nodiscard]] std::vector<std::wstring> shell_drop_paths(
    IDataObject* data) noexcept;

// Receives shell drags onto the host window and turns them into imports into
// the volume the page is showing.
//
// The two halves of a drop never meet anywhere else: the page names the
// destination and knows nothing about the source, the shell hands over sources
// and knows nothing about the destination, and only this object holds both.
// What crosses back to the page is four numbers.
//
// Reference counted, because `RegisterDragDrop` keeps a reference of its own.
// Create it with `new`, hand it to `RegisterDragDrop`, and release the initial
// reference when the window is going away.
class ExternalDropTarget final : public IDropTarget,
                                 public IUiExternalDropTargetSink {
 public:
  // `work` is the session exclusion every other worker uses, so a drop cannot
  // start while the page owns the session and the page cannot call in while a
  // drop is running.
  ExternalDropTarget(ExternalImportStep step, CreationWorkState& work) noexcept;

  ExternalDropTarget(const ExternalDropTarget&) = delete;
  ExternalDropTarget& operator=(const ExternalDropTarget&) = delete;

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** object) noexcept override;
  ULONG STDMETHODCALLTYPE AddRef() noexcept override;
  ULONG STDMETHODCALLTYPE Release() noexcept override;

  // IDropTarget
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD key_state,
                                      POINTL point,
                                      DWORD* effect) noexcept override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL point,
                                     DWORD* effect) noexcept override;
  HRESULT STDMETHODCALLTYPE DragLeave() noexcept override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state,
                                 POINTL point, DWORD* effect) noexcept override;

  // IUiExternalDropTargetSink
  bool set_target(std::uint32_t id,
                  std::wstring_view relative_directory) noexcept override;
  void clear_target() noexcept override;
  bool request_cancel() noexcept override;
  [[nodiscard]] ExternalDropStatus status() const noexcept override;

  // Stops a run in flight and waits for it. The window is going away, so the
  // session the worker is holding is about to go with it.
  void close() noexcept;

 private:
  ~ExternalDropTarget();

  // Whether a drag arriving now would be taken. Asked on every `DragOver`, so
  // the cursor answers while the user is still deciding.
  [[nodiscard]] bool accepts(IDataObject* data) const noexcept;
  void run(std::uint32_t id, std::wstring relative_directory,
           std::vector<std::wstring> sources) noexcept;

  ExternalImportStep step_;
  CreationWorkState& work_;
  std::atomic<ULONG> references_{1};

  // The destination, as the page last named it. Written on the UI thread and
  // read there too, but a run in flight took its own copy, so clearing this
  // never redirects a copy that is already going.
  mutable std::mutex mutex_;
  std::uint32_t target_id_{};
  std::wstring target_directory_;

  std::atomic<bool> armed_{false};
  std::atomic<bool> accepting_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<unsigned int> state_{0};
  std::atomic<unsigned int> total_{0};
  std::atomic<unsigned int> completed_{0};
  std::atomic<unsigned int> failed_{0};
  std::thread worker_;
};

}  // namespace usb_vault::ui
