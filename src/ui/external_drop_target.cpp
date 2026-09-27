#include "usb_vault/ui/external_drop_target.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <memory>

#include <algorithm>
#include <utility>

namespace usb_vault::ui {
namespace {

// Publication states the page reads. They are the vocabulary the rest of the
// bridge already uses for a run: nothing yet, going, done, some of it failed,
// stopped.
constexpr unsigned int kIdle = 0;
constexpr unsigned int kRunning = 1;
constexpr unsigned int kSucceeded = 2;
constexpr unsigned int kFailed = 3;
constexpr unsigned int kCancelled = 4;

// One drag can name a great many files. The limit is not a storage bound --
// the copy would handle them -- but a bound on how long the window can stay
// unable to answer the page, and on how large a mistaken drag can get before
// the user notices what is happening.
constexpr UINT kMaximumDroppedEntries = 1024;

// Holds a medium for exactly as long as the paths are being read out of it.
class MediumRelease final {
 public:
  explicit MediumRelease(STGMEDIUM& medium) noexcept : medium_(medium) {}
  ~MediumRelease() noexcept { ::ReleaseStgMedium(&medium_); }

  MediumRelease(const MediumRelease&) = delete;
  MediumRelease& operator=(const MediumRelease&) = delete;

 private:
  STGMEDIUM& medium_;
};

[[nodiscard]] FORMATETC hdrop_format() noexcept {
  return FORMATETC{.cfFormat = CF_HDROP,
                   .ptd = nullptr,
                   .dwAspect = DVASPECT_CONTENT,
                   .lindex = -1,
                   .tymed = TYMED_HGLOBAL};
}

}  // namespace

std::vector<std::wstring> shell_drop_paths(IDataObject* const data) noexcept {
  std::vector<std::wstring> paths;
  if (data == nullptr) return paths;
  FORMATETC format = hdrop_format();
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium))) return paths;
  const MediumRelease release(medium);
  if (medium.tymed != TYMED_HGLOBAL || medium.hGlobal == nullptr) return paths;

  const auto drop = static_cast<HDROP>(medium.hGlobal);
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
  if (count == 0 || count > kMaximumDroppedEntries) return paths;
  try {
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
      if (length == 0) continue;
      std::wstring path(length, L'\0');
      // The count excludes the terminator, the call writes one, so the buffer
      // handed over is one longer than the string that comes back.
      const UINT written =
          ::DragQueryFileW(drop, index, path.data(), length + 1);
      if (written == 0) continue;
      path.resize(written);
      paths.push_back(std::move(path));
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    paths.clear();
  }
  return paths;
}

ExternalDropTarget::ExternalDropTarget(ExternalImportStep step,
                                       CreationWorkState& work) noexcept
    : step_(std::move(step)), work_(work) {}

ExternalDropTarget::~ExternalDropTarget() {
  close();
}

HRESULT STDMETHODCALLTYPE ExternalDropTarget::QueryInterface(
    REFIID iid, void** const object) noexcept {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (iid == IID_IUnknown || iid == IID_IDropTarget) {
    *object = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ExternalDropTarget::AddRef() noexcept {
  return references_.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE ExternalDropTarget::Release() noexcept {
  const ULONG remaining = references_.fetch_sub(1) - 1;
  if (remaining == 0) delete this;
  return remaining;
}

bool ExternalDropTarget::accepts(IDataObject* const data) const noexcept {
  if (!armed_.load() || data == nullptr) return false;
  // A drop while anything else owns the session would have to wait for it, and
  // a drag that is going to wait should say so by refusing now.
  if (work_.running.load() || work_.interrupted.load()) return false;
  FORMATETC format = hdrop_format();
  return data->QueryGetData(&format) == S_OK;
}

HRESULT STDMETHODCALLTYPE ExternalDropTarget::DragEnter(
    IDataObject* const data, DWORD, POINTL, DWORD* const effect) noexcept {
  const bool accepted = accepts(data);
  accepting_.store(accepted);
  hover(accepted ? 3U : 0U);
  if (effect != nullptr) *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE ExternalDropTarget::DragOver(
    DWORD, POINTL, DWORD* const effect) noexcept {
  hover(accepting_.load() && armed_.load() && !work_.running.load() && !work_.interrupted.load() ? 3U : 0U);
  if (effect != nullptr) {
    *effect = accepting_.load() && armed_.load() && !work_.running.load() && !work_.interrupted.load()
        ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE ExternalDropTarget::DragLeave() noexcept {
  hover(0);
  accepting_.store(false);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE ExternalDropTarget::Drop(
    IDataObject* const data, DWORD, POINTL, DWORD* const effect) noexcept {
  if (effect != nullptr) *effect = DROPEFFECT_NONE;
  hover(0);
  const bool accepted = accepting_.exchange(false);
  // 🔴 Checked again rather than trusted from `DragEnter`: the page may have
  // left maintenance, and the card may have gone, while the cursor was still
  // over the window.
  if (!accepted || !accepts(data)) return S_OK;

  std::vector<std::wstring> sources = shell_drop_paths(data);
  if (sources.empty()) return S_OK;

  if (start_sources(std::move(sources)) && effect != nullptr) *effect = DROPEFFECT_COPY;
  return S_OK;
}

LRESULT CALLBACK ExternalDropTarget::drop_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* target = reinterpret_cast<ExternalDropTarget*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    target = static_cast<ExternalDropTarget*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(target));
  }
  if (target && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK)) {
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    ::ClientToScreen(window, &point);
    const bool button = target->over_button(point);
    if (message == WM_MOUSEMOVE) {
      TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
      ::TrackMouseEvent(&tracking);
      target->hover(button ? 2U : 1U);
    } else if ((message == WM_LBUTTONUP && button) || (message == WM_LBUTTONDBLCLK && !button)) {
      target->hover(0);
      static_cast<void>(target->select_files());
    }
    return 0;
  }
  if (message == WM_MOUSELEAVE && target) { target->hover(0); return 0; }
  if (message == WM_MOUSEWHEEL) return ::SendMessageW(::GetParent(window), message, wparam, lparam);
  return ::DefWindowProcW(window, message, wparam, lparam);
}

bool ExternalDropTarget::attach_window(HWND parent) noexcept {
  if (drop_window_ || !::IsWindow(parent)) return false;
  WNDCLASSW type{};
  type.style = CS_DBLCLKS;
  type.lpfnWndProc = drop_window_proc;
  type.hInstance = ::GetModuleHandleW(nullptr);
  type.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
  type.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
  type.lpszClassName = L"UsbVaultNativeDropRegion";
  if (!::RegisterClassW(&type) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  drop_window_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, type.lpszClassName, L"",
      WS_CHILD, 0, 0, 0, 0, parent, nullptr, type.hInstance, this);
  // A nonzero alpha retains native hit testing without obscuring the HTML.
  if (!drop_window_ || !::SetLayeredWindowAttributes(drop_window_, 0, 1, LWA_ALPHA) ||
      FAILED(::RegisterDragDrop(drop_window_, this))) {
    if (drop_window_) ::DestroyWindow(drop_window_);
    drop_window_ = nullptr; return false;
  }
  return true;
}

void ExternalDropTarget::hover(unsigned state) noexcept {
  if (state == hover_state_) return;
  hover_state_ = state;
  try { if (hover_callback_) hover_callback_(state); } catch (...) { USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));}
}

bool ExternalDropTarget::over_button(POINT screen) const noexcept {
  if (!drop_window_) return false;
  ::ScreenToClient(::GetParent(drop_window_), &screen);
  return ::PtInRect(&button_bounds_, screen) != FALSE;
}

bool ExternalDropTarget::set_drop_button_bounds(int x, int y, int width, int height) noexcept {
  button_bounds_ = {};
  RECT client{};
  if (!drop_window_ || !::GetClientRect(::GetParent(drop_window_), &client) ||
      x < 0 || y < 0 || width < 0 || height < 0 || x > client.right || y > client.bottom ||
      width > client.right-x || height > client.bottom-y) return false;
  button_bounds_ = {x, y, x+width, y+height};
  return true;
}

bool ExternalDropTarget::set_drop_bounds(int x, int y, int width, int height) noexcept {
  if (!drop_window_) return false;
  if (!armed_.load() || work_.running.load() || work_.interrupted.load() || width == 0 || height == 0) {
    hover(0);
    ::ShowWindow(drop_window_, SW_HIDE); return true;
  }
  RECT client{};
  if (!::GetClientRect(::GetParent(drop_window_), &client) || x < 0 || y < 0 || width < 0 || height < 0 ||
      x > client.right || y > client.bottom || width > client.right-x || height > client.bottom-y) {
    ::ShowWindow(drop_window_, SW_HIDE); return false;
  }
  return ::SetWindowPos(drop_window_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
}

bool ExternalDropTarget::select_files() noexcept {
  if (!armed_.load() || work_.running.load() || work_.interrupted.load()) return false;
  const auto epoch = work_.card_epoch.load();
  try {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options |
        FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM |
        FOS_DONTADDTORECENT | FOS_NOCHANGEDIR))) return false;
    static_cast<void>(dialog->SetTitle(L"选择要放入的文件"));
    if (dialog->Show(::GetActiveWindow()) != S_OK || work_.card_epoch.load() != epoch) return false;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    DWORD count=0;
    if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count)) || count==0 || count>kMaximumDroppedEntries) return false;
    std::vector<std::wstring> sources;
    sources.reserve(count);
    for(DWORD n=0;n<count;++n) {
      Microsoft::WRL::ComPtr<IShellItem> item;PWSTR raw=nullptr;
      if (FAILED(items->GetItemAt(n,&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&raw))) return false;
      const std::unique_ptr<wchar_t,decltype(&::CoTaskMemFree)> path(raw, &::CoTaskMemFree);
      sources.emplace_back(path.get());
    }
    if(work_.card_epoch.load()!=epoch) return false;
    return start_sources(std::move(sources));
  } catch(...) { USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return false; }
}

bool ExternalDropTarget::start_sources(std::vector<std::wstring> sources) noexcept {
  try {
  if (!armed_.load() || work_.running.load() || work_.interrupted.load() || sources.empty() || sources.size()>kMaximumDroppedEntries) return false;
  std::uint32_t id = 0;
  std::wstring directory;
  {
    const std::lock_guard lock(mutex_);
    id = target_id_;
    directory = target_directory_;
  }
  if (id == 0) return false;

  // The shell's drag loop and the bridge are both on this thread, so the claim
  // below cannot race the page; the worker is what makes it necessary at all.
  if (worker_.joinable()) worker_.join();
  cancelled_.store(false);
  state_.store(kRunning);
  total_.store(static_cast<unsigned int>(sources.size()));
  completed_.store(0);
  failed_.store(0);
  file_percent_.store(0);
  work_.finish();  // A drop is not the creation gate's kind of cancellable.
  work_.volume_operation.store(true);
  work_.running.store(true);
  if (drop_window_) ::ShowWindow(drop_window_, SW_HIDE);
  try {
    worker_ = std::thread(&ExternalDropTarget::run, this, id,
                          std::move(directory), std::move(sources));
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    state_.store(kFailed);
    work_.running.store(false);
    return false;
  }
  return true;
  } catch (...) { USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return false; }
}

void ExternalDropTarget::run(const std::uint32_t id,
                             const std::wstring relative_directory,
                             const std::vector<std::wstring> sources) noexcept {
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const std::uint64_t epoch = work_.card_epoch.load();
  const auto stop = [this, epoch] {
    return cancelled_.load() || work_.interrupted.load() ||
           work_.card_epoch.load() != epoch;
  };

  unsigned int completed = 0;
  unsigned int failed = 0;
  for (const std::wstring& source : sources) {
    if (stop()) break;
    try {
      file_percent_.store(0);
      if (step_) step_(id, relative_directory, source, stop,
          [this](unsigned int percent) { file_percent_.store((std::min)(percent,99U)); });
      ++completed;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      // A failed protected write may leave metadata awaiting recovery.
      // Never advance to another source on the same maintenance mount.
      ++failed;
      failed_.store(failed);
      break;
    }
    file_percent_.store(0);
    completed_.store(completed);
    failed_.store(failed);
  }

  // A stopped run says so even when everything it managed to copy succeeded:
  // what matters to the user is that the rest was not attempted.
  state_.store(stop() ? kCancelled : failed > 0 ? kFailed : kSucceeded);
  if (SUCCEEDED(com)) ::CoUninitialize();
  work_.running.store(false);
}

bool ExternalDropTarget::set_target(
    const std::uint32_t id,
    const std::wstring_view relative_directory) noexcept {
  if (id == 0) return false;
  // Naming a destination retires the run before it, so a finished one is read
  // exactly once and two identical drops are never mistaken for each other.
  // A run still going keeps its numbers: this only moves where the next lands.
  if (state_.load() != kRunning) {
    state_.store(kIdle);
    total_.store(0);
    completed_.store(0);
    failed_.store(0);
  }
  try {
    const std::lock_guard lock(mutex_);
    target_id_ = id;
    target_directory_.assign(relative_directory);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    clear_target();
    return false;
  }
  armed_.store(true);
  return true;
}

void ExternalDropTarget::clear_target() noexcept {
  armed_.store(false);
  hover(0);
  if (drop_window_) ::ShowWindow(drop_window_, SW_HIDE);
  accepting_.store(false);
  try {
    const std::lock_guard lock(mutex_);
    target_id_ = 0;
    target_directory_.clear();
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/external_drop_target", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    // The window is disarmed either way, which is what refusing a drop needs.
  }
}

bool ExternalDropTarget::request_cancel() noexcept {
  if (state_.load() != kRunning) return false;
  cancelled_.store(true);
  return true;
}

ExternalDropStatus ExternalDropTarget::status() const noexcept {
  // The state is read first because the worker writes it last: a caller that
  // sees a finished run is therefore looking at that run's final counts, and
  // one that sees a running drop may only be a moment behind it.
  const unsigned int state = state_.load();
  const unsigned int total = total_.load();
  const unsigned int completed = completed_.load();
  const unsigned int failed = failed_.load();
  return ExternalDropStatus{.state = state,
                            .total = total,
                            .completed = (std::min)(completed, total),
                            .failed = (std::min)(failed, total)};
}

void ExternalDropTarget::close() noexcept {
  if (drop_window_) {
    ::RevokeDragDrop(drop_window_);
    ::DestroyWindow(drop_window_);
    drop_window_ = nullptr;
  }
  clear_target();
  cancelled_.store(true);
  if (worker_.joinable()) worker_.join();
  // The window is going away, so there is no run for the page to read about.
  state_.store(kIdle);
}

}  // namespace usb_vault::ui
