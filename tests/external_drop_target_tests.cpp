#include "usb_vault/ui/external_drop_target.h"

#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using usb_vault::ui::CreationWorkState;
using usb_vault::ui::ExternalDropStatus;
using usb_vault::ui::ExternalDropTarget;
using usb_vault::ui::shell_drop_paths;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

// A data object holding exactly what a shell drag holds: one `CF_HDROP` block
// of double-null-terminated paths, or a format this product does not take.
class FakeDataObject final : public IDataObject {
 public:
  explicit FakeDataObject(const std::vector<std::wstring>& paths,
                          const CLIPFORMAT format = CF_HDROP)
      : format_(format) {
    if (paths.empty()) return;
    std::wstring block;
    for (const std::wstring& path : paths) {
      block.append(path);
      block.push_back(L'\0');
    }
    block.push_back(L'\0');
    const std::size_t bytes =
        sizeof(DROPFILES) + block.size() * sizeof(wchar_t);
    memory_ = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory_ == nullptr) throw std::runtime_error("out of memory");
    auto* const header = static_cast<DROPFILES*>(::GlobalLock(memory_));
    *header = DROPFILES{.pFiles = sizeof(DROPFILES),
                        .pt = POINT{0, 0},
                        .fNC = FALSE,
                        .fWide = TRUE};
    std::memcpy(static_cast<std::byte*>(static_cast<void*>(header)) +
                    sizeof(DROPFILES),
                block.data(), block.size() * sizeof(wchar_t));
    static_cast<void>(::GlobalUnlock(memory_));
  }

  ~FakeDataObject() {
    if (memory_ != nullptr) static_cast<void>(::GlobalFree(memory_));
  }

  FakeDataObject(const FakeDataObject&) = delete;
  FakeDataObject& operator=(const FakeDataObject&) = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** object) noexcept override {
    if (object == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IDataObject) {
      *object = static_cast<IDataObject*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 2; }
  ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }

  HRESULT STDMETHODCALLTYPE QueryGetData(
      FORMATETC* const format) noexcept override {
    if (format == nullptr) return E_INVALIDARG;
    return format->cfFormat == format_ && memory_ != nullptr ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* const format,
                                    STGMEDIUM* const medium) noexcept override {
    if (format == nullptr || medium == nullptr) return E_INVALIDARG;
    ++get_data_calls;
    if (format->cfFormat != format_ || memory_ == nullptr) {
      return DV_E_FORMATETC;
    }
    // Handed over without a releaser, so the caller's `ReleaseStgMedium` does
    // not free the block this object still owns.
    *medium = STGMEDIUM{
        .tymed = TYMED_HGLOBAL, .hGlobal = memory_, .pUnkForRelease = nullptr};
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,
                                        STGMEDIUM*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
      FORMATETC*, FORMATETC*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*,
                                    BOOL) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(
      DWORD, IEnumFORMATETC**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                    DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) noexcept override {
    return E_NOTIMPL;
  }

  unsigned int get_data_calls{};

 private:
  CLIPFORMAT format_;
  HGLOBAL memory_{};
};

// Everything the session was asked to import, and a way to hold one import
// open so a run can be observed while it is still going.
struct ImportLog final {
  struct Call final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring source;
  };

  std::mutex mutex;
  std::vector<Call> calls;
  std::wstring refuse_source;
  std::wstring hold_source;
  std::atomic<bool> holding{false};
  std::atomic<bool> release_hold{false};
  std::atomic<bool> hold_saw_cancellation{false};

  [[nodiscard]] std::vector<Call> snapshot() {
    const std::lock_guard lock(mutex);
    return calls;
  }
};

[[nodiscard]] usb_vault::ui::ExternalImportStep step_into(ImportLog& log) {
  return [&log](const std::uint32_t id,
                const std::wstring_view relative_directory,
                const std::wstring_view absolute_source,
                const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress) {
    {
      const std::lock_guard lock(log.mutex);
      log.calls.push_back({.id = id,
                           .directory = std::wstring(relative_directory),
                           .source = std::wstring(absolute_source)});
    }
    if (!log.hold_source.empty() && absolute_source == log.hold_source) {
      progress(42);
      log.holding.store(true);
      while (!log.release_hold.load()) {
        if (cancelled && cancelled()) log.hold_saw_cancellation.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      log.holding.store(false);
    }
    if (!log.refuse_source.empty() && absolute_source == log.refuse_source) {
      throw std::runtime_error("the volume would not take it");
    }
  };
}

// Owns the initial reference the way the window does.
class TargetHolder final {
 public:
  TargetHolder(usb_vault::ui::ExternalImportStep step, CreationWorkState& work)
      : target_(new ExternalDropTarget(std::move(step), work)) {}
  ~TargetHolder() {
    target_->close();
    static_cast<void>(target_->Release());
  }

  TargetHolder(const TargetHolder&) = delete;
  TargetHolder& operator=(const TargetHolder&) = delete;

  [[nodiscard]] ExternalDropTarget& operator*() const noexcept {
    return *target_;
  }
  [[nodiscard]] ExternalDropTarget* operator->() const noexcept {
    return target_;
  }

 private:
  ExternalDropTarget* target_;
};

[[nodiscard]] DWORD drag_over(ExternalDropTarget& target, IDataObject& data) {
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  static_cast<void>(target.DragEnter(&data, 0, POINTL{0, 0}, &effect));
  DWORD hovering = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  static_cast<void>(target.DragOver(0, POINTL{0, 0}, &hovering));
  require(effect == hovering, "the cursor changed its answer without a move");
  return effect;
}

[[nodiscard]] DWORD drop(ExternalDropTarget& target, IDataObject& data) {
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  static_cast<void>(target.DragEnter(&data, 0, POINTL{0, 0}, &effect));
  DWORD dropped = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  static_cast<void>(target.Drop(&data, 0, POINTL{0, 0}, &dropped));
  return dropped;
}

void wait_until_idle(CreationWorkState& work) {
  for (int spin = 0; spin < 5000 && work.running.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(!work.running.load(), "a drop never gave the session back");
}

void test_a_drop_carries_every_path_the_shell_named() {
  CreationWorkState work;
  ImportLog log;
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\Users\\pictures\\2011.jpg", L"D:\\scans"});

  require(target->set_target(7, L"photos/2011"), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY,
          "an armed window refused a drop it should take");
  wait_until_idle(work);

  const auto calls = log.snapshot();
  require(calls.size() == 2, "the drop did not carry both entries");
  require(calls[0].id == 7 && calls[0].directory == L"photos/2011" &&
              calls[0].source == L"C:\\Users\\pictures\\2011.jpg",
          "the first entry lost its source or its destination");
  require(calls[1].source == L"D:\\scans",
          "the second entry did not reach the session");

  const ExternalDropStatus status = target->status();
  require(status.state == 2 && status.total == 2 && status.completed == 2 &&
              status.failed == 0,
          "a finished drop did not report itself as done");
}

void test_the_window_takes_a_drop_only_when_the_page_armed_it() {
  CreationWorkState work;
  ImportLog log;
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\a\\b.txt"});

  // Nothing is armed until the maintenance page says which container and
  // directory it is showing, so the cursor says no over the whole window.
  require(drag_over(*target, data) == DROPEFFECT_NONE,
          "an unarmed window offered to take a drop");
  require(drop(*target, data) == DROPEFFECT_NONE, "an unarmed window took one");

  require(target->set_target(3, L""), "the page could not arm it");
  require(drag_over(*target, data) == DROPEFFECT_COPY,
          "an armed window refused the drag");

  // Leaving maintenance disarms it again, and a drag already over the window
  // is re-checked at the drop rather than trusted from the way in.
  DWORD effect = DROPEFFECT_COPY;
  static_cast<void>(target->DragEnter(&data, 0, POINTL{0, 0}, &effect));
  require(effect == DROPEFFECT_COPY, "the drag was refused on the way in");
  target->clear_target();
  DWORD dropped = DROPEFFECT_COPY;
  static_cast<void>(target->Drop(&data, 0, POINTL{0, 0}, &dropped));
  require(dropped == DROPEFFECT_NONE,
          "a drop landed after the page left maintenance");

  require(!target->set_target(0, L""),
          "the window armed itself on a container nothing answers to");
  require(drop(*target, data) == DROPEFFECT_NONE,
          "the window took a drop for an identifier it refused");
  require(log.snapshot().empty(), "a refused drop still reached the session");
  require(!work.running.load(), "a refused drop claimed the session");
}

void test_only_a_shell_file_list_is_taken() {
  CreationWorkState work;
  ImportLog log;
  TargetHolder target(step_into(log), work);
  require(target->set_target(3, L""), "the page could not arm it");

  // 🔴 A drag carrying anything else -- a virtual file that would have to be
  // written to an ordinary disk before it could be copied in -- is refused.
  FakeDataObject rendered({L"C:\\a\\b.txt"}, CF_TEXT);
  require(drag_over(*target, rendered) == DROPEFFECT_NONE,
          "the window offered to take a format it cannot accept");
  require(drop(*target, rendered) == DROPEFFECT_NONE,
          "the window took a format it cannot accept");
  require(shell_drop_paths(&rendered).empty(),
          "paths were read out of a format that has none");
  require(shell_drop_paths(nullptr).empty(),
          "paths were read out of nothing at all");

  FakeDataObject empty({});
  require(drop(*target, empty) == DROPEFFECT_NONE,
          "the window took a drag carrying no files");
  require(log.snapshot().empty(), "a refused drop still reached the session");
}

void test_a_drop_owns_the_session_while_it_runs() {
  CreationWorkState work;
  ImportLog log;
  log.hold_source = L"C:\\a\\big.bin";
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\a\\big.bin", L"C:\\a\\small.txt"});
  require(target->set_target(5, L""), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY, "the drop was refused");

  for (int spin = 0; spin < 5000 && !log.holding.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(log.holding.load(), "the run never reached the entry it holds");
  require(target->file_progress()==42 && target->status().completed==0,"in-flight progress lost or counted as completion");
  require(work.running.load(), "a running drop did not claim the session");
  require(work.volume_operation.load(),
          "a running drop was not declared a volume operation");

  // A second drag while one is running is refused rather than queued: the
  // session is held by the worker and the page is already saying so.
  FakeDataObject second({L"C:\\a\\other.txt"});
  require(drag_over(*target, second) == DROPEFFECT_NONE,
          "a second drag was offered while one was running");

  const ExternalDropStatus running = target->status();
  require(running.state == 1 && running.total == 2,
          "a running drop did not report what it was working through");

  log.release_hold.store(true);
  wait_until_idle(work);
  require(log.snapshot().size() == 2, "the run stopped after the held entry");
  require(target->status().state == 2, "the finished run did not say so");
}

void test_naming_the_next_destination_retires_the_run_before_it() {
  CreationWorkState work;
  ImportLog log;
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\a\\small.txt"});
  require(target->set_target(2, L""), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY, "the drop was refused");
  wait_until_idle(work);
  require(target->status().state == 2, "the drop did not finish");

  // The page reads a finished run once and then names where the next one
  // would land. Two identical drops must not read as one result.
  require(target->set_target(2, L"photos"), "the page could not re-arm it");
  const ExternalDropStatus idle = target->status();
  require(idle.state == 0 && idle.total == 0 && idle.completed == 0 &&
              idle.failed == 0,
          "a finished run survived the next destination being named");

  // A run still going keeps its numbers: naming a destination only decides
  // where the entries after it land.
  log.hold_source = L"C:\\a\\held.bin";
  FakeDataObject held({L"C:\\a\\held.bin", L"C:\\a\\after.txt"});
  require(drop(*target, held) == DROPEFFECT_COPY, "the second drop was refused");
  for (int spin = 0; spin < 5000 && !log.holding.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(log.holding.load(), "the run never reached the entry it holds");
  require(target->set_target(2, L"elsewhere"), "the page could not re-arm it");
  require(target->status().state == 1 && target->status().total == 2,
          "naming a destination discarded a run that was still going");
  log.release_hold.store(true);
  wait_until_idle(work);
}

void test_a_drop_reports_what_the_volume_would_not_take() {
  CreationWorkState work;
  ImportLog log;
  log.refuse_source = L"C:\\a\\second.txt";
  TargetHolder target(step_into(log), work);
  FakeDataObject data(
      {L"C:\\a\\first.txt", L"C:\\a\\second.txt", L"C:\\a\\third.txt"});
  require(target->set_target(9, L"archive"), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY, "the drop was refused");
  wait_until_idle(work);

  // A protected-write failure stops all later entries until recovery.
  require(log.snapshot().size() == 2, "the queue continued after a refusal");
  const ExternalDropStatus status = target->status();
  require(status.state == 3 && status.total == 3 && status.completed == 1 &&
              status.failed == 1,
          "the drop did not report exactly what did not arrive");
}

void test_a_running_drop_stops_when_it_is_asked_to() {
  CreationWorkState work;
  ImportLog log;
  log.hold_source = L"C:\\a\\big.bin";
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\a\\big.bin", L"C:\\a\\next.txt"});

  require(!target->request_cancel(), "an idle window accepted a cancellation");
  require(target->set_target(5, L""), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY, "the drop was refused");
  for (int spin = 0; spin < 5000 && !log.holding.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(log.holding.load(), "the run never reached the entry it holds");

  require(target->request_cancel(), "a running drop refused a cancellation");
  for (int spin = 0; spin < 5000 && !log.hold_saw_cancellation.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(log.hold_saw_cancellation.load(),
          "the cancellation never reached the entry in flight");
  log.release_hold.store(true);
  wait_until_idle(work);

  require(log.snapshot().size() == 1,
          "a cancelled run went on to the next entry");
  require(target->status().state == 4, "a cancelled run did not say so");
}

void test_a_card_that_goes_stops_the_run_and_the_window() {
  CreationWorkState work;
  ImportLog log;
  log.hold_source = L"C:\\a\\big.bin";
  TargetHolder target(step_into(log), work);
  FakeDataObject data({L"C:\\a\\big.bin", L"C:\\a\\next.txt"});
  require(target->set_target(5, L""), "the page could not arm it");
  require(drop(*target, data) == DROPEFFECT_COPY, "the drop was refused");
  for (int spin = 0; spin < 5000 && !log.holding.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(log.holding.load(), "the run never reached the entry it holds");

  // The card leaving is the session ending, so the rest of the drop is not
  // attempted and the window stops offering to take another one.
  work.interrupted.store(true);
  log.release_hold.store(true);
  wait_until_idle(work);
  require(log.snapshot().size() == 1, "the run continued without a card");
  require(target->status().state == 4, "an interrupted run did not say so");
  require(drag_over(*target, data) == DROPEFFECT_NONE,
          "the window offered to take a drop without a card");
}

}  // namespace

int main() {
  try {
    test_a_drop_carries_every_path_the_shell_named();
    test_the_window_takes_a_drop_only_when_the_page_armed_it();
    test_only_a_shell_file_list_is_taken();
    test_a_drop_owns_the_session_while_it_runs();
    test_naming_the_next_destination_retires_the_run_before_it();
    test_a_drop_reports_what_the_volume_would_not_take();
    test_a_running_drop_stops_when_it_is_asked_to();
    test_a_card_that_goes_stops_the_run_and_the_window();
    std::cout << "external_drop_target_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "external_drop_target_tests failed: " << error.what() << '\n';
    return 1;
  }
}
