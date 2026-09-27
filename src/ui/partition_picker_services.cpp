#include "usb_vault/ui/card_product_webview_host_object.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include <Windows.h>
#include <stdexcept>
#include <shellapi.h>
#include <array>
#include <string>

namespace usb_vault::ui {
namespace {
class NativePartitionServices final : public IUiPartitionServices {
 public:
  NativePartitionServices() = default;
  explicit NativePartitionServices(const std::filesystem::path& helper)
      : creator_(std::make_unique<volume_creation::VolumeCreator>(helper)) {}
  void check(const volume::PartitionCandidate& candidate) override {
    if (!creator_) throw std::runtime_error("partition helper is not configured");
    creator_->check_partition(candidate);
  }
  volume::PartitionBitLockerStatus inspect_bitlocker(const volume::PartitionCandidate& candidate) override {
    if (!creator_) throw std::runtime_error("partition helper is not configured");
    return creator_->inspect_partition_bitlocker(candidate);
  }
  void decrypt_bitlocker(const volume::PartitionCandidate& candidate, const std::function<bool()>& confirm,
                          const std::function<bool()>& cancelled) override {
    if (!creator_) throw std::runtime_error("partition helper is not configured");
    creator_->decrypt_partition_bitlocker(candidate, confirm, cancelled);
  }
  bool open_bitlocker_help() noexcept override {
    return reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open",
        L"https://support.microsoft.com/en-us/windows/security/encryption/bitlocker-overview",
        nullptr, nullptr, SW_SHOWNORMAL)) > 32;
  }
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
    } catch (...) { USB_VAULT_TRACE_FAILURE("ui/partition_picker_services", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return false; }
  }
 private:
  std::unique_ptr<volume_creation::VolumeCreator> creator_;
};
}
std::unique_ptr<IUiPartitionServices> make_partition_services(const std::filesystem::path& helper) {
  return std::make_unique<NativePartitionServices>(helper);
}
IUiPartitionServices& native_partition_services() noexcept {
  static NativePartitionServices services;
  return services;
}
}  // namespace usb_vault::ui
