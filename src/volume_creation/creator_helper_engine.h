#pragma once

#include "volume_creation/creator_protocol.h"

#include <Windows.h>

namespace usb_vault::volume_creation::detail {

void execute_request(HANDLE pipe, Request& request);

}  // namespace usb_vault::volume_creation::detail
