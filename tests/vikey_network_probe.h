#pragma once

#include <cstdint>

namespace usb_vault::card::vikey::test {

// Exercises TCP and UDP over IPv4 and IPv6 against loopback and documentation
// addresses. Returns zero only when every supported path is rejected with
// WSAEACCES or an immediately invalidated fresh socket and at least one path
// was exercised. Other values distinguish Winsock initialization failure, no
// supported address family, or the first path that was not denied. The
// ordinary-network predicate succeeds only for that final case.
[[nodiscard]] std::uint32_t network_probe_diagnostic();
[[nodiscard]] bool all_network_paths_denied();
[[nodiscard]] bool ordinary_network_path_available();

}  // namespace usb_vault::card::vikey::test
