#pragma once

#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

namespace usb_vault::volume::detail {
namespace {

constexpr auto kRetryWindow = std::chrono::seconds(2);
constexpr auto kRetryInterval = std::chrono::milliseconds(100);

}  // namespace

template <typename Provider>
UnmountResult retry_unmount_silently(const Provider& provider,
                                 const wchar_t drive_letter) {
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + kRetryWindow;
  std::uint32_t attempts = 0;

  for (;;) {
    ++attempts;
    try {
      UnmountResult result = provider.unmount(
          drive_letter, {.force = false, .notify_shell = false});
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

template <typename Provider>
UnmountEscalationResult escalate_unmount_silently(
    const Provider& provider, const wchar_t drive_letter) {
  const auto start = std::chrono::steady_clock::now();
  try {
    UnmountResult result = provider.unmount(
        drive_letter, {.force = false, .notify_shell = false});
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
      drive_letter, {.force = true, .notify_shell = false});
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  result.attempts = 2;
  return {.result = result, .forced = true};
}

template <typename Provider>
UnmountResult retry_unmount(const Provider& provider, wchar_t drive_letter,
                            const UnmountRetryOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  UnmountResult result;
  provider.run_unmount_batch(drive_letter, options.notify_shell, [&] {
    result = retry_unmount_silently(provider, drive_letter);
  });
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  return result;
}

template <typename Provider>
UnmountEscalationResult escalate_unmount(const Provider& provider,
    wchar_t drive_letter, const UnmountRetryOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  UnmountEscalationResult result;
  provider.run_unmount_batch(drive_letter, options.notify_shell, [&] {
    result = escalate_unmount_silently(provider, drive_letter);
  });
  result.result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  return result;
}

}  // namespace usb_vault::volume::detail
