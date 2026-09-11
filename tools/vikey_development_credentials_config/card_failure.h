#include "card/diagnostic_error.h"
#pragma once
#include "checks.h"
#include "usb_vault/card/card_error.h"
#include <system_error>

namespace configuration {
// Only fixed text and numeric codes reach the UI. Never forward exception
// messages, credential values, card contents or device identities.
inline Result card_failure(const std::error_code& error) {
  using usb_vault::card::CardError;
  if (error.category() != usb_vault::card::card_error_category()) {
    return {State::failed, L"[SYSTEM-" + std::to_wstring(error.value()) +
        L"] 卡检查发生系统错误。请提供此编号及软件版本；不能据此判断凭据错误。"};
  }
  const wchar_t* message = L"未识别的卡访问错误。请提供此编号及软件版本。";
  switch (static_cast<CardError>(error.value())) {
    case CardError::backend_unavailable: message = L"辅助程序启动或运行失败。请重新解压完整测试包，并检查安全软件的拦截记录；不要关闭防护。"; break;
    case CardError::invalid_configuration: message = L"卡访问配置或参数不符合要求。请重新保存全部三项凭据；仍失败请提供此编号及软件版本。"; break;
    case CardError::device_not_found: message = L"未找到匹配的安全卡，请检查插卡和 UserSN。"; break;
    case CardError::authentication_failed: message = L"用户 PIN 错误，请核对后重新保存。"; break;
    case CardError::authentication_locked: message = L"用户 PIN 认证已锁定。请停止重试，联系供应方解锁；改填正确 PIN 不会自动解除锁定。"; break;
    case CardError::permission_denied: message = L"卡操作权限不足。请确认用户权限及卡配置符合要求，联系供应方核对。"; break;
    case CardError::unsupported: message = L"当前卡或 SDK 不支持所需操作。请向供应方核对卡型号和配套软件版本。"; break;
    case CardError::communication_failed: message = L"卡或辅助程序通信失败。请退出其他用卡程序，直连 USB 后重试；开发卡访问还需检查网络。此结果不代表密钥错误。"; break;
    case CardError::invalid_operation: message = L"SDK 操作参数或调用状态无效。请退出其他用卡程序后重试；仍失败请提供此编号及软件版本。"; break;
    case CardError::device_failure: message = L"SDK 或设备返回未细分故障。请提供此编号及软件版本，需进一步诊断；不要据此修改 PIN 或重新初始化。"; break;
    case CardError::api_key_invalid: message = L"API Key 校验失败，请核对后重新保存。"; break;
    case CardError::api_key_missing: message = L"SDK 报告尚未设置 API Key。请重新保存对应 API Key；仍失败请联系供应方核对配置。"; break;
    case CardError::api_key_calculation_failed: message = L"API Key 计算阶段失败。请核对 API Key 与开发配置是否对应，并检查网络；不能仅凭此错误认定输入值错误。"; break;
    case CardError::api_key_verification_failed: message = L"API Key 校验失败，请核对后重新保存。"; break;
    case CardError::user_sn_invalid: message = L"UserSN 校验失败，请核对后重新保存。"; break;
    case CardError::activation_required: message = L"SDK 报告卡未激活。请联系供应方确认激活状态；无需反复修改 PIN。"; break;
    case CardError::multiple_devices: message = L"SDK 发现多张安全卡。请只保留需要使用的一张卡后重新检测。"; break;
    case CardError::operation_timed_out: message = L"等待辅助程序响应超时。请检查网络、代理以及辅助程序是否被拦截；不要连续重试 PIN。"; break;
    default: break;
  }
  return {State::failed, L"[CARD-" + std::to_wstring(error.value()) + L"] " + message};
}
inline Result card_failure(const std::system_error& error) {
  using namespace usb_vault::card;
  auto result = card_failure(error.code());
  const auto* diagnostic = dynamic_cast<const DiagnosticError*>(&error);
  if (!diagnostic || diagnostic->stage() == DiagnosticStage::none) return result;
  const wchar_t* label = L"未知阶段";
  const wchar_t* specific = nullptr;
  switch (diagnostic->stage()) {
    case DiagnosticStage::credential_sn: label = L"读取 UserSN"; specific = L"读取 UserSN 失败，请重新保存凭据。"; break;
    case DiagnosticStage::credential_key: label = L"读取 API Key"; specific = L"读取 API Key 失败，请重新保存凭据。"; break;
    case DiagnosticStage::credential_pin: label = L"读取用户 PIN"; specific = L"读取用户 PIN 失败，请重新保存凭据。"; break;
    case DiagnosticStage::work_directory: label = L"创建运行目录"; specific = L"无法创建运行目录，请检查当前用户目录权限。"; break;
    case DiagnosticStage::path_check: label = L"检查运行路径"; specific = L"运行路径未通过安全检查，请使用普通本地目录；仍失败请联系支持。"; break;
    case DiagnosticStage::file_open: label = L"打开运行文件"; specific = L"无法打开运行文件，请检查文件是否存在、权限及拦截记录。"; break;
    case DiagnosticStage::file_hash: label = L"校验组件摘要"; specific = L"组件摘要校验失败，请重新解压完整包；仍失败请联系支持。"; break;
    case DiagnosticStage::file_state: label = L"检查组件文件状态"; specific = L"组件文件状态异常，请重新解压完整包。"; break;
    case DiagnosticStage::sdk_stage: label = L"准备 SDK 文件"; specific = L"无法准备 SDK 文件，请检查目录权限及拦截记录。"; break;
    case DiagnosticStage::process_start: label = L"启动辅助程序"; specific = L"辅助程序启动失败，请检查权限及拦截记录。"; break;
    case DiagnosticStage::process_security: label = L"配置安全运行环境"; specific = L"无法建立安全运行环境，请联系支持。"; break;
    case DiagnosticStage::sdk_load: label = L"加载 SDK"; specific = L"SDK 加载失败，请检查组件及运行依赖。"; break;
    case DiagnosticStage::sdk_export: label = L"检查 SDK 接口"; specific = L"SDK 缺少所需接口，请使用完整同版测试包。"; break;
    case DiagnosticStage::process_exit: label = L"等待辅助程序退出"; specific = L"辅助程序未正常完成，请提供此编号及退出码。"; break;
    case DiagnosticStage::cleanup: label = L"清理临时文件"; specific = L"临时文件清理失败，请退出其他用卡程序后重试。"; break;
    case DiagnosticStage::ipc_setup: label = L"创建通信通道"; specific = L"无法创建通信通道，请检查系统权限。"; break;
    case DiagnosticStage::ipc_read: label = L"读取辅助程序响应"; specific = L"读取辅助程序响应失败，请联系支持。"; break;
    case DiagnosticStage::ipc_protocol: label = L"校验通信协议"; specific = L"通信数据或版本不匹配，请使用完整同版测试包。"; break;
    case DiagnosticStage::response_timeout: label = L"等待辅助程序响应"; specific = L"等待辅助程序响应超时，请检查网络及拦截记录。"; break;
    case DiagnosticStage::data_timeout: label = L"等待辅助程序数据"; specific = L"等待辅助程序数据超时，请检查设备连接及拦截记录。"; break;
    case DiagnosticStage::sdk_identity: label = L"配置 UserSN"; break;
    case DiagnosticStage::sdk_find: label = L"查找安全卡"; break;
    case DiagnosticStage::sdk_key: label = L"配置 API Key"; break;
    case DiagnosticStage::sdk_attempts: label = L"读取 PIN 剩余次数"; break;
    case DiagnosticStage::sdk_pin: label = L"认证用户 PIN"; break;
    case DiagnosticStage::sdk_permission: label = L"检查卡权限"; break;
    case DiagnosticStage::sdk_read: label = L"读取安全卡"; break;
    case DiagnosticStage::sdk_write: label = L"写入安全卡"; break;
    case DiagnosticStage::sdk_capacity: label = L"读取卡容量"; break;
    case DiagnosticStage::sdk_hardware: label = L"检查卡身份"; break;
    case DiagnosticStage::input_shape: label = L"检查输入格式"; break;
    case DiagnosticStage::sdk_calculation1: label = L"API Key 计算阶段一"; break;
    case DiagnosticStage::sdk_calculation2: label = L"API Key 计算阶段二"; break;
    case DiagnosticStage::installer_signature: label = L"校验安装器签名"; specific = L"安装器签名校验失败，请检查系统时间、证书验证网络及安装器来源。"; break;
    case DiagnosticStage::installer_start: label = L"启动安装器"; specific = L"安装器启动失败，请检查权限及拦截记录。"; break;
    case DiagnosticStage::installer_wait: label = L"读取安装结果"; specific = L"无法获取安装结果，请检查安装器窗口并重新检测。"; break;
    case DiagnosticStage::application_start: label = L"启动主程序"; specific = L"主程序启动失败，请检查权限及拦截记录。"; break;
    case DiagnosticStage::unexpected: label = L"未分类内部异常"; specific = L"发生未分类内部异常，请提供此编号和软件版本。"; break;
    default: break;
  }
  if (specific) result.detail = specific;
  // A bad shape and a missing value require different actions even for one field.
  if (diagnostic->stage() >= DiagnosticStage::credential_sn &&
      diagnostic->stage() <= DiagnosticStage::credential_pin &&
      error.code() == CardError::invalid_configuration)
    result.detail = L"凭据格式无效，请按要求重新保存。";
  if (diagnostic->stage() >= DiagnosticStage::credential_sn &&
      diagnostic->stage() <= DiagnosticStage::credential_pin &&
      diagnostic->kind() == NativeCodeKind::win32 && diagnostic->native() == 1168)
    result.detail = L"当前用户未保存此项凭据，请填写全部三项后保存。";
  if (diagnostic->kind() == NativeCodeKind::win32 && diagnostic->native() == 5)
    result.detail = L"访问被拒绝。" + result.detail;
  if (diagnostic->stage() == DiagnosticStage::file_open &&
      diagnostic->kind() == NativeCodeKind::win32 &&
      (diagnostic->native() == 2 || diagnostic->native() == 3))
    result.detail = L"文件或目录不存在，请重新解压完整包。";
  std::wstring prefix = L"[CARD-" + std::to_wstring(error.code().value()) +
      L"/D" + std::to_wstring(static_cast<std::uint32_t>(diagnostic->stage()));
  if (diagnostic->kind() != NativeCodeKind::none) {
    const wchar_t* kind = diagnostic->kind() == NativeCodeKind::win32 ? L" Win32=" :
        diagnostic->kind() == NativeCodeKind::hresult ? L" HRESULT=" : L" Exit=";
    prefix += kind + std::to_wstring(diagnostic->native());
  }
  prefix += L"] ";
  // Strip the existing top-level prefix before adding a more specific one.
  if (result.detail.starts_with(L"[CARD-")) result.detail.erase(0, result.detail.find(L"] ") + 2);
  result.detail = prefix + label + L"：" + result.detail;
  return result;
}
}  // namespace configuration
