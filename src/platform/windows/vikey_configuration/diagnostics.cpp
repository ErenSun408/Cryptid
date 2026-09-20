#include <Windows.h>
#include <objbase.h>
#include <WebView2.h>
#include "diagnostics.h"
#include "card_failure.h"
#include "p2_deployment.h"
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
Result fail(const wchar_t* text, const std::uint32_t code = 1106,
            const std::uint32_t kind = 0, const std::uint32_t native = 0) {
  return {State::failed, text, {.primary = {.code = code, .native_kind = kind, .native_code = native}}};
}
Result caught(const wchar_t* text, const std::uint32_t fallback) {
  return {State::failed, text, usb_vault::card::capture_startup_failure(fallback)};
}
auto pin(const std::filesystem::path& root, const wchar_t* name, const wchar_t* hash) {
  return usb_vault::card::verify_local_artifact(root / name,
      dev::parse_deployment_digest(hash), 128ULL * 1024 * 1024);
}
}

std::filesystem::path executable_directory() {
  std::wstring path(32768, L'\0');
  const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) usb_vault::card::throw_startup_failure(1002, 1,
      length ? ERROR_INSUFFICIENT_BUFFER : ::GetLastError());
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

bool diagnose(const Report& report, const bool include_card) {
  const auto root = executable_directory();
  std::vector<usb_vault::card::VerifiedLocalArtifact> artifacts;
  dev::WindowsCredentialProvider credentials;
  Probes probes{
    [] {
      SYSTEM_INFO system{}; ::GetNativeSystemInfo(&system);
      return system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64
          ? pass(L"Windows x64；测试支持范围为 Windows 11。")
          : fail(L"当前测试包需要 Windows x64。请使用匹配的设备。", 1101);
    },
    [&] {
      const wchar_t* component = L"运行组件";
      const auto check = [&](const wchar_t* name, const wchar_t* digest) {
        component = name;
        return pin(root, name, digest);
      };
      try {
        // The executable cannot pin a digest of itself, so the check covers
        // the reviewed files beside it.
        artifacts.push_back(check( L"usb_vault_vikey_helper.exe", p2_deployment::helper));
        artifacts.push_back(check( L"usb_vault_volume_creator.exe", p2_deployment::creator));
        artifacts.push_back(check( L"VeraCryptFormat.dll", p2_deployment::format));
        artifacts.push_back(check( L"vendor\\ViKey.dll", p2_deployment::vendor));
        artifacts.push_back(check( L"card_session.html", p2_deployment::html));
        artifacts.push_back(check( L"card_session.js", p2_deployment::js));
        return pass(L"运行组件完整，版本一致。");
      } catch (const std::system_error&) {
        auto result = caught(L"运行组件检查失败，请使用完整安装包修复。", 1102);
        result.detail = std::wstring(component) + L"：" + result.detail;
        return result;
      } catch (...) { return caught(L"组件检查异常，请重新解压完整包。", 1102); }
    },
    [] {
      LPWSTR version{};
      const HRESULT result = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
      const bool available = SUCCEEDED(result) && version && *version;
      ::CoTaskMemFree(version);
      return available ? pass(L"已安装 WebView2 Runtime。")
          : fail(L"WebView2 Runtime 不可用或无法查询。请重新运行 USB Vault 安装程序修复依赖。", 1103,
                 FAILED(result) ? 2 : 0, FAILED(result) ? static_cast<std::uint32_t>(result) : 0);
    },
    [] {
      try {
        return usb_vault::volume::VolumeProvider{}.driver_version() == 0x126
            ? pass(L"VeraCrypt 驱动可访问，接口版本兼容。")
            : fail(L"VeraCrypt 驱动接口不兼容。请重新运行 USB Vault 安装程序修复依赖。", 1105);
      } catch (...) { return caught(L"VeraCrypt 驱动未加载或不可访问。请重启 Windows，或重新运行 USB Vault 安装程序。", 1104); }
    },
    [&] {
      try { static_cast<void>(credentials.load_credentials()); return pass(L"当前用户已保存格式有效的凭据；有效性由安全卡检查确认。"); }
      catch (...) {
        auto result = caught(L"凭据读取失败，请检查配置。", 1108);
        if (result.failure.primary.native_kind == 1 && result.failure.primary.native_code == ERROR_NOT_FOUND)
          result.failure.primary.code = 1107;
        return result;
      }
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
          default: return fail(L"[CARD-STATE] 卡状态检查未通过。请联系供应方检查卡兼容性或状态；本次未改写卡，不要直接重新初始化。");
        }
      } catch (const std::system_error& error) {
        return card_failure(error);
      } catch (...) { return fail(L"[CARD-UNKNOWN] 卡检查发生未分类异常。请提供此编号及软件版本；不要据此修改 PIN 或重新初始化。"); }
    }
  };
  return run_checks(probes, report, include_card);
}

StartupInspection inspect_startup() {
  try {
    std::array<Result, count> results{};
    static_cast<void>(diagnose([&](const unsigned int index, const Result& result) {
      if (index < count) results[index] = result;
    }, false));
    StartupInspection inspection{true, false, {}};
    for (unsigned int index = 0;
         index <= static_cast<unsigned int>(Check::driver); ++index) {
      if (results[index].state == State::passed) continue;
      inspection.dependencies_ready = false;
      inspection.detail = results[index].detail;
      inspection.failure = results[index].failure;
      if (!inspection.failure.primary.code) inspection.failure.primary.code = 1106;
      break;
    }
    inspection.credentials_ready =
        results[static_cast<unsigned int>(Check::credentials)].state ==
        State::passed;
    inspection.credential_failure = results[static_cast<unsigned int>(Check::credentials)].failure;
    return inspection;
  } catch (...) {
    // A start check that cannot run is not a passing one.
    return {false, false, L"启动检查无法完成。请重新运行 USB Vault 安装程序，或重新解压完整测试包。",
            usb_vault::card::capture_startup_failure(1106)};
  }
}
}
