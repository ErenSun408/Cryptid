#pragma once

#include <cstdint>

namespace usb_vault::card::vikey::test {

// Exercises TCP and UDP over IPv4 and IPv6 against loopback and documentation
// addresses. Returns zero only when every supported path is rejected with
// WSAEACCES or an immediately invalidated fresh socket and at least one path
// was exercised. A nonzero value encodes the first path and Winsock result that
// was not denied, for fixture diagnostics.
[[nodiscard]] std::uint32_t network_probe_diagnostic();
[[nodiscard]] bool all_network_paths_denied();

}  // namespace usb_vault::card::vikey::test
