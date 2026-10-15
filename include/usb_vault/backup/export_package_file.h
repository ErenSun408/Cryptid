#pragma once

#include "usb_vault/backup/export_manifest_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <functional>
#include <string_view>

namespace usb_vault::backup {

// Exports one file or directory as an encrypted package at
// `absolute_destination`, a file path that must not exist yet.
//
// The package is written under a random name beside the destination, flushed,
// and only then renamed into place without replacement, so the destination
// either holds a complete package or does not exist. Whatever fails -- the
// walk, the room check, a changed source, a full disk, cancellation, an
// occupied name -- the partial file is deleted before the error leaves. A
// crash can still strand the temporary file; it holds only ciphertext that
// cannot be told apart from random bytes.
//
// Throws std::system_error with ERROR_FILE_EXISTS for an occupied
// destination, ERROR_DISK_FULL when the package cannot fit, ERROR_CANCELLED
// when `cancelled` asked to stop, and the errors of
// collect_export_manifest_v1 and write_export_package_v1 otherwise.
void export_package_to_file_v1(std::wstring_view absolute_source,
                               std::wstring_view absolute_destination,
                               const security::SecureBuffer& password_utf8,
                               const std::function<bool()>& cancelled = {},
                               const ExportProgress& progress = {});

}  // namespace usb_vault::backup
