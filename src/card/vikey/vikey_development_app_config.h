#pragma once

#include "card/vikey/vikey_helper_backend.h"

#include <span>
#include <string_view>

namespace usb_vault::card::vikey::development {

inline constexpr std::wstring_view
    kNetworkEnabledDevelopmentAcknowledgement =
        L"--i-understand-vikey-development-mode-is-network-enabled";

// Parses the complete process argument vector for the separate development
// application. All values are non-secret deployment data. Relative paths,
// malformed digests, missing acknowledgement, and extra arguments fail closed.
[[nodiscard]] VikeyHelperBackendConfig parse_app_config(
    std::span<const std::wstring_view> arguments);

// Only the P2 development executable opts into this sibling-file deployment.
// Digests must come from its compiled build identity, never a runtime manifest.
[[nodiscard]] Sha256Digest parse_deployment_digest(std::wstring_view value);
[[nodiscard]] VikeyHelperBackendConfig packaged_app_config(
    const std::filesystem::path& executable_directory,
    std::wstring_view helper_digest, std::wstring_view vendor_digest);

}  // namespace usb_vault::card::vikey::development
