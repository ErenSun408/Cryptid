#include "platform/windows/shell_container_file_chooser.h"

#include "usb_vault/ui/hidden_items_reveal.h"

#include <ShObjIdl_core.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <combaseapi.h>

#include <iterator>
#include <span>
#include <string>
#include <system_error>

namespace usb_vault::platform::windows {
namespace {

constexpr wchar_t kContainerDirectoryTitle[] = L"选择安全盘的保存位置";
constexpr wchar_t kExistingContainerTitle[] = L"选择要导入的安全盘文件或密文导出包";
// The generic import takes containers and cipher export packages alike, and
// neither has a fixed extension, so every file is offered.
constexpr COMDLG_FILTERSPEC kContainerFilter[] = {
    {L"所有文件", L"*.*"},
};
constexpr wchar_t kContactKeyTitle[] = L"选择联系人公钥文件";
constexpr wchar_t kContactKeySaveTitle[] = L"保存本人公钥文件";
constexpr wchar_t kContactKeyDefaultName[] = L"我的公钥.uvkey";
constexpr COMDLG_FILTERSPEC kContactKeyFilter[] = {
    {L"联系人公钥文件", L"*.uvkey"},
    {L"所有文件", L"*.*"},
};

// What one dialog differs in. Key files are never hidden, so only the
// container pick reveals Hidden+System items.
struct DialogSetup final {
  const CLSID& dialog_class;
  FILEOPENDIALOGOPTIONS options;
  const wchar_t* title;
  std::span<const COMDLG_FILTERSPEC> filter;
  bool reveal_hidden;
  const wchar_t* file_name;
  const wchar_t* default_extension;
};

// Neither dialog may leave a trace of the chosen location. `FOS_DONTADDTORECENT`
// keeps the selection out of the recent-documents list, and never calling
// `SetClientGuid` keeps the dialog from persisting its own last-visited folder
// under a private key. `FOS_FORCESHOWHIDDEN` lists plain hidden items whatever
// Explorer is set to show; Hidden+System containers need the temporary reveal
// around `Show`.
constexpr FILEOPENDIALOGOPTIONS kSharedOptions =
    FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR |
    FOS_PATHMUSTEXIST | FOS_FORCESHOWHIDDEN;

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

// The folder picker has no filter, where a file type filter and a default
// extension are meaningless and rejected.
[[nodiscard]] std::wstring run_dialog(const HWND owner, const DialogSetup& setup) {
  ComPtr<IFileDialog> dialog;
  HRESULT result = ::CoCreateInstance(setup.dialog_class, nullptr,
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
  result = dialog->SetOptions(defaults | kSharedOptions | setup.options);
  if (SUCCEEDED(result)) result = dialog->SetTitle(setup.title);
  if (SUCCEEDED(result) && !setup.filter.empty()) {
    result = dialog->SetFileTypes(static_cast<UINT>(setup.filter.size()), setup.filter.data());
  }
  if (SUCCEEDED(result) && setup.file_name != nullptr) result = dialog->SetFileName(setup.file_name);
  if (SUCCEEDED(result) && setup.default_extension != nullptr) {
    result = dialog->SetDefaultExtension(setup.default_extension);
  }
  if (FAILED(result)) {
    throw_dialog_failure(result, "container file dialog rejected its setup");
  }
  // Every chooser opens on the Desktop. Without one the dialog keeps its own
  // starting folder rather than failing.
  ComPtr<IShellItem> desktop;
  if (SUCCEEDED(::SHGetKnownFolderItem(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr,
                                       IID_PPV_ARGS(desktop.put())))) {
    static_cast<void>(dialog->SetFolder(desktop.get()));
  }

  if (setup.reveal_hidden) {
    // Hidden+System containers are only listed while Explorer shows protected
    // system files; the switches are restored as soon as the dialog closes.
    const auto reveal = ui::shell_hidden_items_revealer().reveal();
    result = dialog->Show(owner);
  } else {
    result = dialog->Show(owner);
  }
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

// The browse button behind the form's save-location field. It picks a folder
// and nothing else: the file name comes from the name the user typed, so a
// save dialog here would ask for it twice and let the two disagree.
std::wstring ShellContainerFileChooser::choose_container_directory() {
  return run_dialog(owner_, {.dialog_class = CLSID_FileOpenDialog,
                             .options = FOS_PICKFOLDERS,
                             .title = kContainerDirectoryTitle,
                             .filter = {},
                             .reveal_hidden = true,
                             .file_name = nullptr,
                             .default_extension = nullptr});
}

std::wstring ShellContainerFileChooser::choose_existing_container() {
  return run_dialog(owner_, {.dialog_class = CLSID_FileOpenDialog,
                             .options = FOS_FILEMUSTEXIST,
                             .title = kExistingContainerTitle,
                             .filter = kContainerFilter,
                             .reveal_hidden = true,
                             .file_name = nullptr,
                             .default_extension = nullptr});
}

std::wstring ShellContainerFileChooser::choose_contact_key_file() {
  return run_dialog(owner_, {.dialog_class = CLSID_FileOpenDialog,
                             .options = FOS_FILEMUSTEXIST,
                             .title = kContactKeyTitle,
                             .filter = kContactKeyFilter,
                             .reveal_hidden = false,
                             .file_name = nullptr,
                             .default_extension = nullptr});
}

std::wstring ShellContainerFileChooser::choose_contact_key_save_path() {
  return run_dialog(owner_, {.dialog_class = CLSID_FileSaveDialog,
                             .options = FOS_OVERWRITEPROMPT,
                             .title = kContactKeySaveTitle,
                             .filter = kContactKeyFilter,
                             .reveal_hidden = false,
                             .file_name = kContactKeyDefaultName,
                             .default_extension = L"uvkey"});
}

}  // namespace usb_vault::platform::windows
