#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include "diagnostics.h"
#include "p2_config_deployment.h"
#include "card/verified_local_artifact.h"
#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_card_enrollment.h"
#include "card/vikey/vikey_hid_probe.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/card/card_error.h"
#include <vector>
#include <stdexcept>

namespace configuration {
namespace dev = usb_vault::card::vikey::development;
namespace {
Result pass(const wchar_t* text) { return {State::passed, text}; }
Result fail(const wchar_t* text) { return {State::failed, text}; }
auto pin(const std::filesystem::path& root, const wchar_t* name, const wchar_t* hash) {
  return usb_vault::card::verify_local_artifact(root / name,
      dev::parse_deployment_digest(hash), 128ULL * 1024 * 1024);
}
}

std::filesystem::path executable_directory() {
  std::wstring path(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) throw std::runtime_error("module path unavailable");
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

bool diagnose(const Report& report) {
  const auto root = executable_directory();
  std::vector<usb_vault::card::VerifiedLocalArtifact> artifacts;
  dev::WindowsCredentialProvider credentials;
  Probes probes{
    [] {
      SYSTEM_INFO system{}; ::GetNativeSystemInfo(&system);
      return system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
          ? pass(L"Windows x64；测试支持范围为 Windows 11。")
          : fail(L"当前测试包需要 Windows x64。请使用匹配的设备。");
    },
    [&] {
      try {
        artifacts.push_back(pin(root, L"usb_vault_vikey_p2_development_app.exe", p2_deployment::app));
        artifacts.push_back(pin(root, L"usb_vault_vikey_helper.exe", p2_deployment::helper));
        artifacts.push_back(pin(root, L"usb_vault_volume_creator.exe", p2_deployment::creator));
        artifacts.push_back(pin(root, L"VeraCryptFormat.dll", p2_deployment::format));
        artifacts.push_back(pin(root, L"vendor\\ViKey.dll", p2_deployment::vendor));
        artifacts.push_back(pin(root, L"card_session.html", p2_deployment::html));
        artifacts.push_back(pin(root, L"card_session.js", p2_deployment::js));
        return pass(L"主程序和运行组件完整，版本一致。");
      } catch (...) { return fail(L"组件缺失、修改或路径不支持；请重新解压完整包到本机固定磁盘。"); }
    },
    [] {
      LPWSTR version{};
      const HRESULT result = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
      const bool available = SUCCEEDED(result) && version && *version;
      ::CoTaskMemFree(version);
      return available ? pass(L"已安装 WebView2 Runtime。")
          : fail(L"未找到 WebView2 Runtime；请按测试须知安装后重试。");
    },
    [] {
      try {
        return usb_vault::volume::VolumeProvider{}.driver_version() == 0x126
            ? pass(L"VeraCrypt 驱动可访问，接口版本兼容。")
            : fail(L"驱动接口不兼容；请按测试须知安装随包提供的 VeraCrypt。");
      } catch (...) { return fail(L"驱动未加载或不可访问；请安装并启动 VeraCrypt 后重试。"); }
    },
    [&] {
      try { static_cast<void>(credentials.load_credentials()); return pass(L"当前用户已保存格式有效的凭据；有效性由安全卡检查确认。"); }
      catch (...) { return fail(L"凭据缺失或格式无效；请填写并保存全部三项。"); }
    },
    [] {
      const auto device = usb_vault::card::vikey::probe_vikey_hid_access();
      if (!device.device_present) return fail(L"未发现安全卡，请插卡后重新检测。");
      return device.read_write_opened ? pass(L"已识别安全卡，设备可访问。")
          : fail(L"安全卡不可访问；请关闭其他用卡程序并检查设备权限。");
    },
    [&] {
      try {
        usb_vault::card::vikey::VikeyHelperCardEnrollment enrollment(
            dev::packaged_app_config(root, p2_deployment::helper, p2_deployment::vendor), credentials);
        auto inspection = enrollment.inspect_inserted_card();
        if (!inspection) return fail(L"无法确认安全卡状态，请检查凭据、网络和卡是否匹配。");
        using namespace usb_vault::application;
        switch (CardEntryRouter::route(*inspection)) {
          case CardEntryRoute::initialization_required: return pass(L"安全卡访问通过；进入主程序后可设置登录密码。");
          case CardEntryRoute::login_required: return pass(L"安全卡访问通过；可进入主程序登录。");
          default: return fail(L"安全卡当前不可用；本次检测未初始化或改写卡。");
        }
      } catch (const std::system_error& error) {
        using usb_vault::card::CardError;
        if (error.code() == CardError::authentication_failed)
          return fail(L"凭据认证失败，请核对供应方提供的配置，不要反复猜测 PIN。");
        if (error.code() == CardError::authentication_locked)
          return fail(L"安全卡认证已锁定，请联系供应方处理。");
        if (error.code() == CardError::device_not_found)
          return fail(L"检测期间安全卡不可用，请重新插卡后重试。");
        return fail(L"安全卡访问失败；请核对凭据、网络和卡是否匹配。");
      } catch (...) { return fail(L"安全卡访问失败；请核对凭据、网络和卡是否匹配。"); }
    }
  };
  return run_checks(probes, report);
}

void launch_application() {
  const auto root = executable_directory();
  const auto app = pin(root, L"usb_vault_vikey_p2_development_app.exe", p2_deployment::app);
  std::wstring command = L"\"" + app.path().native() + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(app.path().c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, root.c_str(), &startup, &process))
    throw std::runtime_error("application launch failed");
  ::CloseHandle(process.hThread); ::CloseHandle(process.hProcess);
}
}
