#include "card/vikey/vikey_development_app_config.h"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace usb_vault::card::vikey::development {
namespace {

[[nodiscard]] unsigned int hex_nibble(const wchar_t value) {
  if (value >= L'0' && value <= L'9') {
    return static_cast<unsigned int>(value - L'0');
  }
  if (value >= L'A' && value <= L'F') {
    return static_cast<unsigned int>(value - L'A' + 10);
  }
  if (value >= L'a' && value <= L'f') {
    return static_cast<unsigned int>(value - L'a' + 10);
  }
  throw std::invalid_argument(
      "ViKey development digest contains a non-hexadecimal character");
}

[[nodiscard]] Sha256Digest parse_digest(const std::wstring_view value) {
  if (value.size() != Sha256Digest{}.size() * 2) {
    throw std::invalid_argument(
        "ViKey development digest must contain exactly 64 hex digits");
  }
  Sha256Digest result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    const unsigned int high = hex_nibble(value[index * 2]);
    const unsigned int low = hex_nibble(value[index * 2 + 1]);
    result[index] = static_cast<std::byte>((high << 4U) | low);
  }
  return result;
}

[[nodiscard]] std::filesystem::path require_absolute_path(
    const std::wstring_view value, const bool require_filename) {
  if (value.empty()) {
    throw std::invalid_argument("ViKey development path is empty");
  }
  std::filesystem::path result{std::wstring(value)};
  if (!result.is_absolute()) {
    throw std::invalid_argument(
        "ViKey development paths must be absolute");
  }
  result = result.lexically_normal();
  if ((require_filename && !result.has_filename()) ||
      (!require_filename && result == result.root_path())) {
    throw std::invalid_argument("ViKey development path shape is invalid");
  }
  return result;
}

}  // namespace

VikeyHelperBlobBackendConfig parse_app_config(
    const std::span<const std::wstring_view> arguments) {
  if (arguments.size() != 6 || arguments[0].empty() ||
      arguments[1] != kNetworkEnabledDevelopmentAcknowledgement) {
    throw std::invalid_argument(
        "usage: usb_vault_vikey_development_app "
        "--i-understand-vikey-development-mode-is-network-enabled "
        "<helper.exe> <helper-sha256> <vendor-dir> <vendor-sha256>");
  }
  return {
      .helper_path = require_absolute_path(arguments[2], true),
      .helper_digest = parse_digest(arguments[3]),
      .vendor_directory = require_absolute_path(arguments[4], false),
      .vendor_digest = parse_digest(arguments[5]),
  };
}

}  // namespace usb_vault::card::vikey::development
