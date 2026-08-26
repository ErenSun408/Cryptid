#pragma once

#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::card::vikey::development {

// Generates a one-shot non-production value directly in page-locked memory.
// The returned buffer owns the only application-side copy and clears it on
// destruction.
[[nodiscard]] security::SecureBuffer random_test_blob();

}  // namespace usb_vault::card::vikey::development
