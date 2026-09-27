#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

namespace usb_vault::volume {
namespace {

constexpr auto kRetryWindow = std::chrono::seconds(2);
constexpr auto kRetryInterval = std::chrono::milliseconds(100);

}  // namespace

UnmountResult unmount_with_retry(const VolumeProvider& provider,
                                 const wchar_t drive_letter,
                                 const UnmountRetryOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + kRetryWindow;
  std::uint32_t attempts = 0;

  for (;;) {
    ++attempts;
    try {
      UnmountResult result = provider.unmount(
          drive_letter, {.force = false, .notify_shell = options.notify_shell});
      result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start);
      result.attempts = attempts;
      return result;
    } catch (const std::system_error& error) {
      if (error.code() != make_error_code(DriverError::volume_busy)) {
        throw;
      }

      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        USB_VAULT_TRACE("卸载", "卷持续占用，重试期限已到",
                        "盘符 " + diagnostics::drive_text(drive_letter) + "；已尝试 " +
                            std::to_string(attempts) + " 次");
        throw;
      }
      USB_VAULT_TRACE("卸载", "卷正忙，稍后重试",
                      "盘符 " + diagnostics::drive_text(drive_letter) + "；第 " +
                          std::to_string(attempts) + " 次");
      std::this_thread::sleep_for(
          std::min(kRetryInterval,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       deadline - now)));
    }
  }
}

UnmountEscalationResult unmount_with_forced_escalation(
    const VolumeProvider& provider, const wchar_t drive_letter,
    const UnmountRetryOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  try {
    UnmountResult result = provider.unmount(
        drive_letter, {.force = false, .notify_shell = options.notify_shell});
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    result.attempts = 1;
    return {.result = result, .forced = false};
  } catch (const std::system_error& error) {
    if (error.code() != make_error_code(DriverError::volume_busy)) {
      throw;
    }
    // The caller explicitly selected escalation (card removal or confirmed
    // user cleanup). Let the forced attempt's own failure propagate.
    USB_VAULT_TRACE("卸载", "卷正忙，升级为强制卸载", "盘符 " + diagnostics::drive_text(drive_letter));
  }

  UnmountResult result = provider.unmount(
      drive_letter, {.force = true, .notify_shell = options.notify_shell});
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  result.attempts = 2;
  return {.result = result, .forced = true};
}

}  // namespace usb_vault::volume
