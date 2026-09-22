#include "usb_vault/ui/card_product_webview_host_object.h"
#include <Windows.h>
#include <shellapi.h>
#include <array>
#include <string>

namespace usb_vault::ui {
namespace {
class NativePartitionServices final : public IUiPartitionServices {
 public:
  std::vector<volume::PartitionCandidate> enumerate() override {
    return volume::enumerate_partition_candidates();
  }
  bool open_disk_management() noexcept override {
    try {
      std::array<wchar_t, MAX_PATH + 1> directory{};
      const UINT count = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
      if (!count || count >= directory.size()) return false;
      const std::wstring base(directory.data(), count);
      const std::wstring executable = base + LR"(\mmc.exe)";
      const std::wstring parameters = L"\"" + base + LR"(\diskmgmt.msc)" + L"\"";
      SHELLEXECUTEINFOW info{sizeof(info)};
      info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
      info.lpVerb = L"open";
      info.lpFile = executable.c_str();
      info.lpParameters = parameters.c_str();
      info.lpDirectory = base.c_str();
      info.nShow = SW_SHOWNORMAL;
      return ::ShellExecuteExW(&info) != FALSE;
    } catch (...) { return false; }
  }
};
}
IUiPartitionServices& native_partition_services() noexcept {
  static NativePartitionServices services;
  return services;
}
}  // namespace usb_vault::ui
