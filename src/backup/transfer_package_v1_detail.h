#pragma once

#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/contacts/contact_identity_v1.h"

#include <span>

// Test access only: the chunk key a recipient recovers from a recipient
// block, which lets tests play a recipient who re-encrypts other content.
namespace usb_vault::backup::detail {

[[nodiscard]] ExportPackageKeyV1 open_transfer_package_key(
    const contacts::ContactIdentity& recipient, std::span<const std::byte> prefix);

}  // namespace usb_vault::backup::detail
