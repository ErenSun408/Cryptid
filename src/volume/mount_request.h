#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include "volume/veracrypt_driver_abi.h"

#include <cstdint>
#include <string_view>

namespace usb_vault::volume::detail {

[[nodiscard]] std::int32_t drive_number(wchar_t drive_letter);
void validate_volume_path(std::wstring_view volume_path);

void populate_mount_request(
    abi::MountRequest& request,
    std::wstring_view volume_path,
    const security::SecureBuffer& volume_password,
    const MountOptions& options);

}  // namespace usb_vault::volume::detail
