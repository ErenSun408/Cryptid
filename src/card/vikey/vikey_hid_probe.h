#pragma once

#include <cstdint>

namespace usb_vault::card::vikey {

struct VikeyHidProbe final {
  bool device_present{};
  bool read_write_opened{};
  std::uint32_t open_error{};
};

// Enumerates only the fixed ViKeyPRO HID interface and attempts to obtain a
// read/write handle without sending any input, output, or feature report.
[[nodiscard]] VikeyHidProbe probe_vikey_hid_access();

}  // namespace usb_vault::card::vikey
