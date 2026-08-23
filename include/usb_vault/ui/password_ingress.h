#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <span>

namespace usb_vault::ui {

// Consumes UTF-8 bytes from a mutable UI bridge allocation. The source bytes
// are securely cleared on every return path, including validation or allocation
// failure. The returned password is owned by a page-locked SecureBuffer.
[[nodiscard]] security::SecureBuffer consume_password_utf8(
    std::span<std::byte> mutable_utf8);

}  // namespace usb_vault::ui
