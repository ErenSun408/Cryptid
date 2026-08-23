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

void validate_options(const MountOptions& options) {
  static_cast<void>(drive_number(options.drive_letter));
  if (options.prf != Pkcs5Prf::sha512) {
    throw std::invalid_argument("only the explicitly validated SHA-512 PRF is allowed");
  }
  if (options.pim < 0 || options.pim > abi::kMaxPimValue) {
    throw std::invalid_argument("PIM is outside the VeraCrypt driver range");
  }
}

}  // namespace

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
  validate_path(volume_path);
  validate_password(volume_password);
  validate_options(options);

  std::wstring driver_path(volume_path);
  std::ranges::replace(driver_path, L'/', L'\\');
  std::ranges::copy(driver_path, request.volume_path);
  for (std::size_t index = 0; index < volume_password.size(); ++index) {
    request.volume_password.text[index] =
        std::to_integer<std::uint8_t>(volume_password.bytes()[index]);
  }
  request.volume_password.length =
      static_cast<std::uint32_t>(volume_password.size());

  request.drive_number = drive_number(options.drive_letter);
  request.bytes_per_sector = 512;
  request.mount_read_only = options.read_only ? 1 : 0;
  request.mount_manager = 1;
  request.preserve_timestamp = 1;
  request.pkcs5_prf = static_cast<std::int32_t>(options.prf);
  request.bytes_per_physical_sector = 512;
  request.volume_pim = options.pim;

  // The zero-initialized ABI buffer intentionally leaves password/PIM caches,
  // hidden-volume protection, recovery, backup headers, and removable mode off.
}

}  // namespace usb_vault::volume::detail
