#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace usb_vault::card::vikey {

struct VikeyHelperProbe final {
  std::uint32_t security_properties{};
};

// Launches the helper under the mandatory process boundary and performs the
// version-1 probe handshake. This does not load ViKey.dll or access a card.
[[nodiscard]] VikeyHelperProbe probe_vikey_helper(
    const std::filesystem::path& helper_path,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

}  // namespace usb_vault::card::vikey
