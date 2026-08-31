#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_client.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using usb_vault::card::vikey::VikeyCardMediaState;

constexpr std::wstring_view kEraseAcknowledgement =
    L"--i-understand-this-permanently-erases-the-p1-development-card";

[[nodiscard]] const char* state_name(const VikeyCardMediaState state) {
  switch (state) {
    case VikeyCardMediaState::blank:
      return "blank";
    case VikeyCardMediaState::legacy_p1_or_foreign_prefix:
      return "legacy-p1-or-foreign-prefix";
    case VikeyCardMediaState::foreign_data:
      return "foreign-data";
    case VikeyCardMediaState::provisioning_interrupted:
      return "provisioning-interrupted";
    case VikeyCardMediaState::conflicting_evidence:
      return "conflicting-evidence";
    case VikeyCardMediaState::product_evidence:
      return "product-evidence";
  }
  return "unknown";
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    if (argc != 7 || argv[6] == nullptr ||
        std::wstring_view(argv[6]) != kEraseAcknowledgement) {
      throw std::invalid_argument(
          "usage: usb_vault_vikey_p1_card_reset "
          "--i-understand-vikey-development-mode-is-network-enabled "
          "<helper.exe> <helper-sha256> <vendor-dir> <vendor-sha256> "
          "--i-understand-this-permanently-erases-the-p1-development-card");
    }

    std::vector<std::wstring_view> arguments;
    arguments.reserve(6);
    for (int index = 0; index < 6; ++index) {
      arguments.emplace_back(argv[index]);
    }
    const auto config =
        usb_vault::card::vikey::development::parse_app_config(arguments);
    usb_vault::card::vikey::development::WindowsCredentialProvider
        credential_provider;
    constexpr auto timeout = std::chrono::seconds(30);

    auto credentials = credential_provider.load_credentials();
    const auto before = usb_vault::card::vikey::
        probe_vikey_development_card_media_with_network_access(
            config.helper_path, config.helper_digest,
            config.vendor_directory, config.vendor_digest,
            credentials.user_sn, credentials.api_key,
            credentials.user_pin, timeout);
    credentials = {};

    if (before.media_state == VikeyCardMediaState::blank) {
      std::cout << std::format(
          "ViKey {:08X} is already blank; no write was performed\n",
          before.hardware_id);
      return 0;
    }
    if (before.media_state !=
        VikeyCardMediaState::legacy_p1_or_foreign_prefix) {
      throw std::runtime_error(std::format(
          "refusing to reset ViKey {:08X}: media state is {}",
          before.hardware_id, state_name(before.media_state)));
    }

    credentials = credential_provider.load_credentials();
    usb_vault::card::vikey::
        reset_vikey_development_legacy_media_with_network_access(
            config.helper_path, config.helper_digest,
            config.vendor_directory, config.vendor_digest,
            before.hardware_id, before.media_state,
            credentials.user_sn, credentials.api_key,
            credentials.user_pin, timeout);
    credentials = {};

    credentials = credential_provider.load_credentials();
    const auto after = usb_vault::card::vikey::
        probe_vikey_development_card_media_with_network_access(
            config.helper_path, config.helper_digest,
            config.vendor_directory, config.vendor_digest,
            credentials.user_sn, credentials.api_key,
            credentials.user_pin, timeout);
    if (after.hardware_id != before.hardware_id ||
        after.media_state != VikeyCardMediaState::blank) {
      throw std::runtime_error(
          "reset completed without a matching blank-card recheck");
    }

    std::cout << std::format(
        "ViKey {:08X} P1 development data was permanently erased; "
        "the card is blank and ready for P2 initialization\n",
        before.hardware_id);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ViKey P1 card reset failed: " << error.what() << '\n';
    return 1;
  }
}
