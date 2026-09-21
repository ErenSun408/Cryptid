#pragma once
#include "usb_vault/volume/volume_provider.h"
#include "volume/veracrypt_driver_abi.h"
#include <algorithm>
#include <stdexcept>

namespace usb_vault::volume::detail {
inline MountIdentity decode_mount_identity(const abi::VolumeProperties& p,
                                           std::int32_t expected_drive) {
  const auto end = std::find(std::begin(p.volume_path), std::end(p.volume_path), L'\0');
  auto boolean = [](std::int32_t value) { return value == 0 || value == 1; };
  if (expected_drive < 0 || expected_drive >= 26 || p.drive_number != expected_drive ||
      p.unique_id < 0 || p.disk_length == 0 || end == std::begin(p.volume_path) ||
      end == std::end(p.volume_path) || !boolean(p.hidden_volume) ||
      !boolean(p.read_only) || !boolean(p.partition_in_inactive_system_scope) ||
      !boolean(p.mount_disabled) || p.hidden_protection < 0 || p.hidden_protection > 2) {
    throw std::invalid_argument("invalid driver mount identity response");
  }
  MountIdentity result{static_cast<wchar_t>(L'A' + expected_drive), p.unique_id,
          {std::begin(p.volume_path), end}, p.disk_length,
          p.hidden_protection == 1 && !p.hidden_volume && !p.read_only &&
          !p.partition_in_inactive_system_scope && !p.mount_disabled &&
          p.header_flags == 0 && p.pkcs5_prf == 1 && p.volume_pim == 0};
  std::copy(std::begin(p.volume_id), std::end(p.volume_id), result.header_digest.begin());
  return result;
}
}  // namespace usb_vault::volume::detail
