#pragma once

#include "volume_creation/creator_protocol.h"

#include <Windows.h>

#include <cstdint>

namespace usb_vault::volume_creation::detail {

// Returns what the request measured, or zero for one that creates.
[[nodiscard]] std::uint64_t execute_request(HANDLE pipe, Request& request);

}  // namespace usb_vault::volume_creation::detail
