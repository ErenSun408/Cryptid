#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_provider.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr char kProbeBytes[] = "usb-vault-p1.1-read-write-probe";

constexpr std::array kFixturePasswordUtf8{
    std::byte{0x70}, std::byte{0xC3}, std::byte{0xA4}, std::byte{0x73},
    std::byte{0x73}, std::byte{0x77}, std::byte{0xC3}, std::byte{0xB6},
    std::byte{0x72}, std::byte{0x64}, std::byte{0x2D}, std::byte{0xE5},
    std::byte{0xAF}, std::byte{0x86}, std::byte{0xE7}, std::byte{0xA0},
    std::byte{0x81}};

constexpr std::array kFixtureCardSecret{
    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
    std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
    std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
    std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
    std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
    std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B},
    std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}};

constexpr usb_vault::security::KdfV1Salt kFixtureSalt{
    std::byte{0xF0}, std::byte{0xF1}, std::byte{0xF2}, std::byte{0xF3},
    std::byte{0xF4}, std::byte{0xF5}, std::byte{0xF6}, std::byte{0xF7},
    std::byte{0xF8}, std::byte{0xF9}, std::byte{0xFA}, std::byte{0xFB},
    std::byte{0xFC}, std::byte{0xFD}, std::byte{0xFE}, std::byte{0xFF}};

[[nodiscard]] wchar_t find_unused_drive_letter() {
  const DWORD drives = ::GetLogicalDrives();
  if (drives == 0) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "GetLogicalDrives");
  }

  for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
    const DWORD mask = 1UL << static_cast<DWORD>(letter - L'A');
    if ((drives & mask) == 0) {
      return letter;
    }
  }
  throw std::runtime_error("no unused integration-test drive letter");
}

[[nodiscard]] std::wstring root_path(const wchar_t drive_letter) {
  std::wstring path{drive_letter};
  path.append(L":\\");
  return path;
}

void write_and_read_probe(const wchar_t drive_letter) {
  std::wstring path = root_path(drive_letter);
  path.append(L"usb-vault-p1.1-test.tmp");

  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "create mount probe");
  }

  DWORD bytes_written = 0;
  const BOOL wrote = ::WriteFile(file, kProbeBytes,
                                 static_cast<DWORD>(sizeof(kProbeBytes)),
                                 &bytes_written, nullptr);
  if (wrote == FALSE) {
    const DWORD error = ::GetLastError();
    static_cast<void>(::CloseHandle(file));
    static_cast<void>(::DeleteFileW(path.c_str()));
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "write mount probe");
  }
  if (bytes_written != sizeof(kProbeBytes)) {
    static_cast<void>(::CloseHandle(file));
    static_cast<void>(::DeleteFileW(path.c_str()));
    throw std::runtime_error("mount probe write was incomplete");
  }

  LARGE_INTEGER start{};
  const BOOL rewound = ::SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
  if (rewound == FALSE) {
    const DWORD error = ::GetLastError();
    static_cast<void>(::CloseHandle(file));
    static_cast<void>(::DeleteFileW(path.c_str()));
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "rewind mount probe");
  }

  char read_buffer[sizeof(kProbeBytes)]{};
  DWORD bytes_read = 0;
  const BOOL read = ::ReadFile(file, read_buffer,
                               static_cast<DWORD>(sizeof(read_buffer)),
                               &bytes_read, nullptr);
  if (read == FALSE) {
    const DWORD error = ::GetLastError();
    static_cast<void>(::CloseHandle(file));
    static_cast<void>(::DeleteFileW(path.c_str()));
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "read mount probe");
  }
  static_cast<void>(::CloseHandle(file));

  if (bytes_read != sizeof(kProbeBytes) ||
      !std::equal(std::begin(kProbeBytes), std::end(kProbeBytes), read_buffer)) {
    static_cast<void>(::DeleteFileW(path.c_str()));
    throw std::runtime_error("mount probe contents did not round-trip");
  }

  if (::DeleteFileW(path.c_str()) == FALSE) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), "delete mount probe");
  }
}

template <std::size_t Size>
[[nodiscard]] usb_vault::security::SecureBuffer secure_copy(
    const std::array<std::byte, Size>& source) {
  usb_vault::security::SecureBuffer destination(source.size());
  std::ranges::copy(source, destination.bytes().begin());
  return destination;
}

[[nodiscard]] usb_vault::security::SecureBuffer fixture_password() {
  const usb_vault::security::SecureBuffer password_utf8 =
      secure_copy(kFixturePasswordUtf8);
  const usb_vault::security::SecureBuffer card_secret =
      secure_copy(kFixtureCardSecret);
  return usb_vault::security::derive_volume_password_v1(
      password_utf8, card_secret, kFixtureSalt);
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  if (argc != 2) {
    std::cerr << "expected one integration container path\n";
    return 2;
  }
  if (::GetFileAttributesW(argv[1]) == INVALID_FILE_ATTRIBUTES) {
    std::cerr << "integration container is unavailable\n";
    return 77;
  }

  usb_vault::volume::VolumeProvider provider;
  const wchar_t drive_letter = find_unused_drive_letter();
  bool mounted = false;
  std::chrono::milliseconds kdf_elapsed{};
  std::chrono::milliseconds mount_elapsed{};
  std::chrono::milliseconds auth_to_mount_elapsed{};
  std::chrono::milliseconds unmount_elapsed{};

  try {
    if (provider.driver_version() != 0x126U) {
      throw std::runtime_error("integration driver version is not 0x126");
    }

    const auto authentication_started = std::chrono::steady_clock::now();
    const auto kdf_started = authentication_started;
    const usb_vault::security::SecureBuffer password = fixture_password();
    kdf_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kdf_started);
    const auto mount_result = provider.mount_file(
        argv[1], password,
        {.drive_letter = drive_letter,
         .prf = usb_vault::volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false});
    mounted = true;
    mount_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        mount_result.elapsed);
    auth_to_mount_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - authentication_started);

    if (auth_to_mount_elapsed >= std::chrono::seconds(5)) {
      throw std::runtime_error(
          "KDF plus single-container mount exceeded five seconds");
    }
    if (::GetDriveTypeW(root_path(drive_letter).c_str()) != DRIVE_FIXED) {
      throw std::runtime_error("mounted drive is not visible as a fixed disk");
    }

    write_and_read_probe(drive_letter);
    const auto mounted_volumes = provider.mounted_volumes();
    if (std::ranges::find_if(mounted_volumes, [&](const auto& volume) {
          return volume.drive_letter == drive_letter;
        }) == mounted_volumes.end()) {
      throw std::runtime_error("mounted volume was absent from driver query");
    }

    const auto unmount_result =
        usb_vault::volume::unmount_with_retry(provider, drive_letter);
    mounted = false;
    unmount_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        unmount_result.elapsed);
    if (unmount_result.elapsed >= std::chrono::seconds(5)) {
      throw std::runtime_error("single-container unmount exceeded five seconds");
    }
    if (::GetDriveTypeW(root_path(drive_letter).c_str()) != DRIVE_NO_ROOT_DIR) {
      throw std::runtime_error("drive remained visible after unmount");
    }

    const auto remaining_volumes = provider.mounted_volumes();
    if (std::ranges::find_if(remaining_volumes, [&](const auto& volume) {
          return volume.drive_letter == drive_letter;
        }) != remaining_volumes.end()) {
      throw std::runtime_error("driver still reported volume after unmount");
    }

    std::cout << "driver=0x126 drive=" << static_cast<char>(drive_letter)
              << " kdf_ms=" << kdf_elapsed.count()
              << " mount_ms=" << mount_elapsed.count()
              << " auth_to_mount_ms=" << auth_to_mount_elapsed.count()
              << " unmount_ms=" << unmount_elapsed.count()
              << " unmount_attempts=" << unmount_result.attempts << '\n';
  } catch (const std::exception& error) {
    if (mounted) {
      try {
        static_cast<void>(provider.unmount(drive_letter, {.force = true}));
      } catch (...) {
      }
    }
    std::cerr << error.what() << '\n';
    return 1;
  }

  return 0;
}
