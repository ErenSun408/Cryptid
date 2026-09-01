#include "platform/windows/shell_container_file_chooser.h"

#include <ShObjIdl_core.h>
#include <combaseapi.h>

#include <iterator>
#include <string>
#include <system_error>

namespace usb_vault::platform::windows {
namespace {

constexpr wchar_t kContainerExtension[] = L"hc";
constexpr wchar_t kNewContainerTitle[] = L"选择加密盘的保存位置";
constexpr wchar_t kExistingContainerTitle[] = L"选择要打开的加密盘文件";
constexpr COMDLG_FILTERSPEC kContainerFilter[] = {
    {L"加密盘文件", L"*.hc"},
};

// Neither dialog may leave a trace of the chosen location. `FOS_DONTADDTORECENT`
// keeps the selection out of the recent-documents list, and never calling
// `SetClientGuid` keeps the dialog from persisting its own last-visited folder
// under a private key.
constexpr FILEOPENDIALOGOPTIONS kSharedOptions =
    FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR |
    FOS_PATHMUSTEXIST;

template <typename T>
class ComPtr final {
 public:
  ComPtr() noexcept = default;
  ~ComPtr() noexcept { reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&&) = delete;
  ComPtr& operator=(ComPtr&&) = delete;

  [[nodiscard]] T** put() noexcept {
    reset();
    return &pointer_;
  }
  [[nodiscard]] T* operator->() const noexcept { return pointer_; }
  [[nodiscard]] T* get() const noexcept { return pointer_; }

  void reset() noexcept {
    if (pointer_ != nullptr) {
      pointer_->Release();
      pointer_ = nullptr;
    }
  }

 private:
  T* pointer_{};
};

class CoTaskPath final {
 public:
  CoTaskPath() noexcept = default;
  ~CoTaskPath() noexcept {
    if (value_ != nullptr) ::CoTaskMemFree(value_);
  }

  CoTaskPath(const CoTaskPath&) = delete;
  CoTaskPath& operator=(const CoTaskPath&) = delete;
  CoTaskPath(CoTaskPath&&) = delete;
  CoTaskPath& operator=(CoTaskPath&&) = delete;

  [[nodiscard]] PWSTR* put() noexcept { return &value_; }
  [[nodiscard]] const wchar_t* get() const noexcept { return value_; }

 private:
  PWSTR value_{};
};

[[noreturn]] void throw_dialog_failure(const HRESULT result,
                                       const char* const message) {
  throw std::system_error(static_cast<int>(result), std::system_category(),
                          message);
}

[[nodiscard]] std::wstring run_dialog(const HWND owner, const CLSID& dialog_class,
                                      const FILEOPENDIALOGOPTIONS options,
                                      const wchar_t* const title) {
  ComPtr<IFileDialog> dialog;
  HRESULT result = ::CoCreateInstance(dialog_class, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(dialog.put()));
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog is unavailable");
  }

  FILEOPENDIALOGOPTIONS defaults{};
  result = dialog->GetOptions(&defaults);
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog rejected a query");
  }
  result = dialog->SetOptions(defaults | kSharedOptions | options);
  if (SUCCEEDED(result)) result = dialog->SetTitle(title);
  if (SUCCEEDED(result)) {
    result = dialog->SetFileTypes(
        static_cast<UINT>(std::size(kContainerFilter)), kContainerFilter);
  }
  if (SUCCEEDED(result)) {
    result = dialog->SetDefaultExtension(kContainerExtension);
  }
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog rejected its setup");
  }

  result = dialog->Show(owner);
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog failed");
  }

  ComPtr<IShellItem> item;
  result = dialog->GetResult(item.put());
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog returned no item");
  }

  CoTaskPath path;
  result = item->GetDisplayName(SIGDN_FILESYSPATH, path.put());
  if (FAILED(result) || path.get() == nullptr) {
    throw_dialog_failure(FAILED(result) ? result : E_UNEXPECTED,
                         "chosen container has no file system path");
  }
  return std::wstring(path.get());
}

}  // namespace

ShellContainerFileChooser::ShellContainerFileChooser(
    const HWND owner) noexcept
    : owner_(owner) {}

std::wstring ShellContainerFileChooser::choose_new_container() {
  return run_dialog(owner_, CLSID_FileSaveDialog,
                    FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN,
                    kNewContainerTitle);
}

std::wstring ShellContainerFileChooser::choose_existing_container() {
  return run_dialog(owner_, CLSID_FileOpenDialog, FOS_FILEMUSTEXIST,
                    kExistingContainerTitle);
}

}  // namespace usb_vault::platform::windows
