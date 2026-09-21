#pragma once

#include "volume_creation/creator_protocol.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb_vault::volume_creation::detail {

// Returns what the request measured, or zero for one that creates.
[[nodiscard]] std::uint64_t execute_request(HANDLE pipe, Request& request);

// Answers measurements on one pipe until the caller closes it, a request that
// is not a measurement arrives, or the session sits idle too long.
void serve_measurement_session(HANDLE pipe,
                               const std::array<std::byte, kNonceSize>& nonce,
                               Request& first);

// Dedicated mount-bound import session; exits on the first failure.
void serve_import_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first);

}  // namespace usb_vault::volume_creation::detail
