#include "volume/mount_request.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace usb_vault::volume::detail {
namespace {

[[nodiscard]] wchar_t uppercase_ascii(const wchar_t value) noexcept {
  if (value >= L'a' && value <= L'z') {
    return static_cast<wchar_t>(value - L'a' + L'A');
  }
  return value;
}

void validate_path(const std::wstring_view volume_path) {
  if (volume_path.empty()) {
    throw std::invalid_argument("volume path must not be empty");
  }
  if (volume_path.size() >= abi::kMaxPath) {
    throw std::invalid_argument("volume path exceeds the driver ABI limit");
  }
  if (std::ranges::find(volume_path, L'\0') != volume_path.end()) {
    throw std::invalid_argument("volume path contains an embedded null");
  }
  if (!std::filesystem::path(volume_path).is_absolute()) {
    throw std::invalid_argument("volume path must be absolute");
  }
}

void validate_password(const security::SecureBuffer& volume_password) {
  if (volume_password.empty()) {
    throw std::invalid_argument("volume password must not be empty");
  }
  if (volume_password.size() > abi::kMaxPassword) {
    throw std::invalid_argument("volume password exceeds the driver ABI limit");
  }
}

void validate_protection(const MountOptions& options) {
  if (!options.hidden_volume_protection.has_value()) {
    return;
  }
  const HiddenVolumeProtection& protection = *options.hidden_volume_protection;
  if (protection.password == nullptr) {
    throw std::invalid_argument(
        "hidden-volume protection needs the password that opens the hidden "
        "volume");
  }
  if (protection.password->empty()) {
    throw std::invalid_argument(
        "hidden-volume protection password must not be empty");
  }
  if (protection.password->size() > abi::kMaxPassword) {
    throw std::invalid_argument(
        "hidden-volume protection password exceeds the driver ABI limit");
  }
  if (protection.prf != Pkcs5Prf::sha512) {
    throw std::invalid_argument(
        "only the explicitly validated SHA-512 PRF is allowed for the "
        "protected hidden volume");
  }
  if (protection.pim < 0 || protection.pim > abi::kMaxPimValue) {
    throw std::invalid_argument(
        "protected hidden volume PIM is outside the VeraCrypt driver range");
  }
}

void validate_options(const MountOptions& options) {
  static_cast<void>(drive_number(options.drive_letter));
  if (options.prf != Pkcs5Prf::sha512) {
    throw std::invalid_argument("only the explicitly validated SHA-512 PRF is allowed");
  }
  if (options.pim < 0 || options.pim > abi::kMaxPimValue) {
    throw std::invalid_argument("PIM is outside the VeraCrypt driver range");
  }
  validate_protection(options);
}

// Both slots are fixed-size and already zeroed, so a shorter password leaves
// the terminator the driver expects.
void copy_password(abi::Password& target,
                   const security::SecureBuffer& password) {
  for (std::size_t index = 0; index < password.size(); ++index) {
    target.text[index] =
        std::to_integer<std::uint8_t>(password.bytes()[index]);
  }
  target.length = static_cast<std::uint32_t>(password.size());
}

}  // namespace

void validate_volume_path(const std::wstring_view volume_path) {
  validate_path(volume_path);
}

std::int32_t drive_number(const wchar_t drive_letter) {
  const wchar_t normalized = uppercase_ascii(drive_letter);
  if (normalized < L'A' || normalized > L'Z') {
    throw std::invalid_argument("drive letter must be in the range A-Z");
  }
  return static_cast<std::int32_t>(normalized - L'A');
}

void populate_mount_request(
    abi::MountRequest& request,
    const std::wstring_view volume_path,
    const security::SecureBuffer& volume_password,
    const MountOptions& options) {
  validate_volume_path(volume_path);
  validate_password(volume_password);
  validate_options(options);

  std::wstring driver_path(volume_path);
  std::ranges::replace(driver_path, L'/', L'\\');
  std::ranges::copy(driver_path, request.volume_path);
  copy_password(request.volume_password, volume_password);

  request.drive_number = drive_number(options.drive_letter);
  request.bytes_per_sector = 512;
  request.mount_read_only = options.read_only ? 1 : 0;
  request.mount_manager = 1;
  request.preserve_timestamp = 1;
  request.pkcs5_prf = static_cast<std::int32_t>(options.prf);
  request.bytes_per_physical_sector = 512;
  request.volume_pim = options.pim;

  // The zero-initialized buffer leaves caches, recovery, backup headers and
  // removable mode off. Protection is the one a caller can ask for.
  if (options.hidden_volume_protection.has_value()) {
    const HiddenVolumeProtection& protection =
        *options.hidden_volume_protection;
    copy_password(request.protected_hidden_volume_password,
                  *protection.password);
    request.protected_hidden_volume_pkcs5_prf =
        static_cast<std::int32_t>(protection.prf);
    request.protected_hidden_volume_pim = protection.pim;
    request.protect_hidden_volume = 1;
  }
}

}  // namespace usb_vault::volume::detail
