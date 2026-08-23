#include "usb_vault/application/mount_orchestrator.h"

#include "usb_vault/volume/unmount_retry.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

constexpr usb_vault::security::KdfV1Salt kSaltA{
    std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
    std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
    std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B},
    std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}};
constexpr usb_vault::security::KdfV1Salt kSaltB{
    std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
    std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
    std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
    std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};

struct MountedState final {
  bool a{false};
  bool b{false};
};

[[nodiscard]] usb_vault::security::SecureBuffer secure_text(
    const std::string_view value) {
  usb_vault::security::SecureBuffer result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(value[index]);
  }
  return result;
}

[[nodiscard]] usb_vault::security::SecureBuffer card_secret() {
  usb_vault::security::SecureBuffer result(
      usb_vault::security::kKdfV1CardSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index);
  }
  return result;
}

[[nodiscard]] std::array<wchar_t, 2> find_unused_drive_letters() {
  const DWORD drives = ::GetLogicalDrives();
  if (drives == 0) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "GetLogicalDrives");
  }

  std::array<wchar_t, 2> result{};
  std::size_t count = 0;
  for (wchar_t letter = L'Z'; letter >= L'D' && count < result.size();
       --letter) {
    const DWORD mask = 1UL << static_cast<DWORD>(letter - L'A');
    if ((drives & mask) == 0) {
      result[count++] = letter;
    }
  }
  if (count != result.size()) {
    throw std::runtime_error("two unused integration-test drives are required");
  }
  return result;
}

[[nodiscard]] std::wstring root_path(const wchar_t drive_letter) {
  std::wstring result{drive_letter};
  result.append(L":\\");
  return result;
}

[[nodiscard]] const usb_vault::volume::MountedVolume& require_volume(
    const std::vector<usb_vault::volume::MountedVolume>& volumes,
    const wchar_t drive_letter) {
  const auto found = std::ranges::find_if(volumes, [&](const auto& volume) {
    return volume.drive_letter == drive_letter;
  });
  if (found == volumes.end()) {
    throw std::runtime_error("mounted drive was absent from driver query");
  }
  return *found;
}

void require_drive_absent(const usb_vault::volume::VolumeProvider& provider,
                          const wchar_t drive_letter) {
  if (::GetDriveTypeW(root_path(drive_letter).c_str()) != DRIVE_NO_ROOT_DIR) {
    throw std::runtime_error("drive remained visible after session cleanup");
  }
  const auto volumes = provider.mounted_volumes();
  if (std::ranges::find_if(volumes, [&](const auto& volume) {
        return volume.drive_letter == drive_letter;
      }) != volumes.end()) {
    throw std::runtime_error("driver retained a cleaned-up session volume");
  }
}

void unmount_session(const usb_vault::volume::VolumeProvider& provider,
                     const std::array<wchar_t, 2>& drives,
                     MountedState& mounted) {
  if (mounted.a) {
    static_cast<void>(usb_vault::volume::unmount_with_retry(provider,
                                                            drives[0]));
    mounted.a = false;
  }
  if (mounted.b) {
    static_cast<void>(usb_vault::volume::unmount_with_retry(provider,
                                                            drives[1]));
    mounted.b = false;
  }
  require_drive_absent(provider, drives[0]);
  require_drive_absent(provider, drives[1]);
}

void emergency_cleanup(const usb_vault::volume::VolumeProvider& provider,
                       const std::array<wchar_t, 2>& drives,
                       MountedState& mounted) noexcept {
  if (mounted.a) {
    try {
      static_cast<void>(provider.unmount(drives[0], {.force = true}));
      mounted.a = false;
    } catch (...) {
    }
  }
  if (mounted.b) {
    try {
      static_cast<void>(provider.unmount(drives[1], {.force = true}));
      mounted.b = false;
    } catch (...) {
    }
  }
}

[[nodiscard]] usb_vault::application::ContainerLayout make_layout(
    const wchar_t* const path_a,
    const wchar_t* const path_b,
    const std::array<wchar_t, 2>& drives) {
  return {
      .container_a = {.path = path_a,
                      .salt = kSaltA,
                      .drive_letter = drives[0]},
      .container_b = {.path = path_b,
                      .salt = kSaltB,
                      .drive_letter = drives[1]},
  };
}

void verify_successful_session(
    const usb_vault::volume::VolumeProvider& provider,
    const usb_vault::application::ContainerLayout& layout,
    const usb_vault::security::SecureBuffer& password,
    const usb_vault::security::SecureBuffer& card,
    const usb_vault::volume::VolumeType expected_b_type,
    MountedState& mounted) {
  const auto result = usb_vault::application::mount_session(
      provider, layout, password, card);
  mounted = {.a = result.container_a.has_value(),
             .b = result.container_b.has_value()};
  if (!mounted.a || !mounted.b) {
    throw std::runtime_error("full layout did not mount two containers");
  }

#ifdef NDEBUG
  if (result.elapsed >= std::chrono::seconds(5)) {
    throw std::runtime_error("double-container session exceeded five seconds");
  }
#endif

  if (::GetDriveTypeW(root_path(layout.container_a.drive_letter).c_str()) !=
          DRIVE_FIXED ||
      ::GetDriveTypeW(root_path(layout.container_b.drive_letter).c_str()) !=
          DRIVE_FIXED) {
    throw std::runtime_error("session drives were not visible as fixed disks");
  }

  const auto volumes = provider.mounted_volumes();
  const auto& volume_a = require_volume(volumes, layout.container_a.drive_letter);
  const auto& volume_b = require_volume(volumes, layout.container_b.drive_letter);
  if (volume_a.type != usb_vault::volume::VolumeType::normal) {
    throw std::runtime_error("container A was not mounted as a normal volume");
  }
  if (volume_b.type != expected_b_type) {
    throw std::runtime_error("container B mounted with the wrong volume type");
  }

  std::cout << "session_b_type=" << static_cast<int>(volume_b.type)
            << " elapsed_ms=" << result.elapsed.count() << '\n';
}

void verify_wrong_password_mounts_nothing(
    const usb_vault::volume::VolumeProvider& provider,
    const usb_vault::application::ContainerLayout& layout,
    const usb_vault::security::SecureBuffer& card) {
  const usb_vault::security::SecureBuffer wrong =
      secure_text("p1.3-wrong-login-password");
  try {
    static_cast<void>(
        usb_vault::application::mount_session(provider, layout, wrong, card));
  } catch (const std::system_error& error) {
    if (error.code() != usb_vault::volume::make_error_code(
                            usb_vault::volume::DriverError::
                                authentication_failed)) {
      throw;
    }
    require_drive_absent(provider, layout.container_a.drive_letter);
    require_drive_absent(provider, layout.container_b.drive_letter);
    return;
  }
  throw std::runtime_error("wrong password unexpectedly mounted a container");
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  if (argc != 3) {
    std::cerr << "expected container A and B integration paths\n";
    return 2;
  }
  if (::GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES ||
      ::GetFileAttributesW(argv[2]) == INVALID_FILE_ATTRIBUTES) {
    std::cerr << "P1.3 integration containers are unavailable\n";
    return 77;
  }

  usb_vault::volume::VolumeProvider provider;
  const auto drives = find_unused_drive_letters();
  const auto layout = make_layout(argv[1], argv[2], drives);
  const usb_vault::security::SecureBuffer card = card_secret();
  MountedState mounted;

  try {
    if (provider.driver_version() != 0x126U) {
      throw std::runtime_error("integration driver version is not 0x126");
    }

    const usb_vault::security::SecureBuffer real =
        secure_text("p1.3-real-login-password");
    verify_successful_session(provider, layout, real, card,
                              usb_vault::volume::VolumeType::hidden, mounted);
    unmount_session(provider, drives, mounted);

    const usb_vault::security::SecureBuffer duress =
        secure_text("p1.3-duress-login-passphrase");
    verify_successful_session(provider, layout, duress, card,
                              usb_vault::volume::VolumeType::normal, mounted);
    unmount_session(provider, drives, mounted);

    verify_wrong_password_mounts_nothing(provider, layout, card);
  } catch (const std::exception& error) {
    emergency_cleanup(provider, drives, mounted);
    std::cerr << error.what() << '\n';
    return 1;
  }

  return 0;
}
