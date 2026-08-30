#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstddef>

namespace usb_vault::security {

// Bounds the untrusted UTF-8 bridge input before any locked conversion
// buffers are allocated. A normalized password accepted by the current
// 128-code-point policy needs at most 512 UTF-8 bytes; the larger bridge cap
// also accommodates canonically decomposed input before NFC composition.
inline constexpr std::size_t kMaximumPasswordUtf8InputBytes = 4096;

// Consumes a non-empty UTF-8 password and returns its Unicode NFC form as
// exact UTF-8 bytes in a new page-locked buffer. All UTF-8/UTF-16 intermediate
// buffers are page-locked and cleared before release; no null-termination is
// assumed, so embedded U+0000 is preserved.
[[nodiscard]] SecureBuffer normalize_password_utf8_nfc(
    SecureBuffer password_utf8);

}  // namespace usb_vault::security
