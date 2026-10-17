#pragma once

#include "usb_vault/volume/volume_provider.h"
#include "volume/locked_driver_buffer.h"
#include "volume/veracrypt_driver_abi.h"

#include <chrono>
#include <stdexcept>
#include <system_error>

namespace usb_vault::volume::detail {

// VeraCrypt 1.26 MountDevice creates its device at the validated nDosDriveNo;
// it does not select a different drive. Once the mount transaction succeeds,
// this number is enough to retain ownership even if an inventory query fails.
// Keep all fallible preparation inside transact_mount and do no further I/O
// between its successful response and handing the result to the caller.
template <typename TransactMount>
[[nodiscard]] MountResult complete_mount(
    LockedDriverBuffer<abi::MountRequest>& locked_request,
    TransactMount&& transact_mount) {
  struct ClearRequest final {
    LockedDriverBuffer<abi::MountRequest>& buffer;
    ~ClearRequest() noexcept { buffer.clear(); }
  } clear{locked_request};
  auto& request = locked_request.get();
  const auto drive = request.drive_number;
  if (drive < 0 || drive >= static_cast<std::int32_t>(abi::kDriveCount))
    throw std::invalid_argument("invalid mount drive number");
  const auto start = std::chrono::steady_clock::now();
  transact_mount(request);
  if (request.return_code != 0)
    throw std::system_error(request.return_code, driver_error_category(),
                            "VeraCrypt mount rejected");
  return {
      .filesystem_dirty = request.filesystem_dirty != 0,
      .read_only_after_access_denied = request.read_only_after_access_denied != 0,
      .read_only_after_device_write_protected = request.read_only_after_device_write_protected != 0,
      .is_ntfs = request.is_ntfs != 0,
      .master_key_vulnerable = request.master_key_vulnerable != 0,
      .driver_set_label = request.driver_set_label != 0,
      .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start),
      .drive_letter = static_cast<wchar_t>(L'A' + drive),
  };
}

}  // namespace usb_vault::volume::detail
