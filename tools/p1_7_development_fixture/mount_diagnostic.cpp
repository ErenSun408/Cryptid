#include "development_container_fixture.h"

#include "card/vikey/vikey_card_store.h"
#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_blob_backend.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/volume_provider.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::wstring_view kDiagnosticAcknowledgement =
    L"--run-disposable-p1.7-real-mount-diagnostic";

[[nodiscard]] std::filesystem::path executable_directory() {
  std::vector<wchar_t> path(32768);
  const DWORD length = ::GetModuleFileNameW(
      nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    throw std::runtime_error("cannot resolve diagnostic directory");
  }
  return std::filesystem::path(std::wstring_view(path.data(), length))
      .parent_path();
}

[[nodiscard]] std::array<wchar_t, 2> unused_drive_letters() {
  const DWORD drives = ::GetLogicalDrives();
  if (drives == 0) {
    throw std::runtime_error("cannot inspect available drive letters");
  }
  std::array<wchar_t, 2> result{};
  std::size_t count = 0;
  for (wchar_t letter = L'Z'; letter >= L'D' && count < result.size();
       --letter) {
    const DWORD mask = 1UL << static_cast<DWORD>(letter - L'A');
    if ((drives & mask) == 0) result[count++] = letter;
  }
  if (count != result.size()) {
    throw std::runtime_error("two unused drive letters are required");
  }
  return result;
}

[[nodiscard]] usb_vault::security::SecureBuffer secure_text(
    const std::string_view value) {
  usb_vault::security::SecureBuffer result(value.size());
  std::ranges::transform(value, result.bytes().begin(), [](const char byte) {
    return static_cast<std::byte>(static_cast<unsigned char>(byte));
  });
  return result;
}

[[nodiscard]] usb_vault::security::SecureBuffer container_a_input() {
  usb_vault::security::SecureBuffer result(
      usb_vault::application::kContainerAPasswordInput.size());
  std::ranges::copy(usb_vault::application::kContainerAPasswordInput,
                    result.bytes().begin());
  return result;
}

void report_system_error(const char* const stage,
                         const std::system_error& error) {
  std::cerr << stage << " failed: category=" << error.code().category().name()
            << " code=" << error.code().value() << " message="
            << error.code().message() << '\n';
}

void unmount_if_needed(const usb_vault::volume::VolumeProvider& provider,
                       const wchar_t drive_letter, bool& mounted,
                       const bool notify_shell = false) {
  if (!mounted) return;
  try {
    static_cast<void>(provider.unmount(
        drive_letter, {.force = false, .notify_shell = notify_shell}));
  } catch (...) {
    static_cast<void>(provider.unmount(
        drive_letter, {.force = true, .notify_shell = notify_shell}));
  }
  mounted = false;
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  bool first_mounted = false;
  bool second_mounted = false;
  wchar_t first_drive = L'\0';
  wchar_t second_drive = L'\0';
  usb_vault::volume::VolumeProvider provider;
  try {
    if (argc != 6 || argv[5] == nullptr ||
        std::wstring_view(argv[5]) != kDiagnosticAcknowledgement) {
      throw std::invalid_argument(
          "usage: diagnostic <helper.exe> <helper-sha256> <vendor-dir> "
          "<vendor-sha256> --run-disposable-p1.7-real-mount-diagnostic");
    }

    const std::array<std::wstring_view, 6> app_arguments{
        argv[0],
        usb_vault::card::vikey::development::
            kNetworkEnabledDevelopmentAcknowledgement,
        argv[1], argv[2], argv[3], argv[4]};
    auto backend_config =
        usb_vault::card::vikey::development::parse_app_config(app_arguments);
    usb_vault::card::vikey::development::WindowsCredentialProvider
        credential_provider;
    usb_vault::card::vikey::VikeyHelperBlobBackend backend(
        std::move(backend_config), credential_provider);
    usb_vault::card::vikey::VikeyCardStore store(backend);

    usb_vault::security::SecureBuffer card_secret;
    try {
      card_secret = store.get_blob();
    } catch (const std::system_error& error) {
      report_system_error("card read", error);
      return 10;
    }
    if (card_secret.size() != usb_vault::security::kKdfV1CardSecretSize) {
      card_secret.clear();
      throw std::runtime_error("card read returned the wrong size");
    }

    try {
      std::cout << "VeraCrypt driver version=" << provider.driver_version()
                << '\n';
    } catch (const std::system_error& error) {
      card_secret.clear();
      report_system_error("driver probe", error);
      return 11;
    }

    const std::filesystem::path directory = executable_directory();
    const std::filesystem::path container_a =
        directory / usb_vault::application::kContainerAFilename;
    const std::filesystem::path container_b =
        directory / usb_vault::application::kContainerBFilename;
    usb_vault::development::require_regular_file(container_a,
                                                 "container A");
    usb_vault::development::require_regular_file(container_b,
                                                 "container B");
    const auto drives = unused_drive_letters();
    first_drive = drives[0];
    second_drive = drives[1];

    usb_vault::security::SecureBuffer real_input = secure_text(
        usb_vault::development::kP17RealTestPassword);
    usb_vault::security::SecureBuffer first_password =
        usb_vault::security::derive_volume_password_v1(
            real_input, card_secret,
            usb_vault::application::kContainerBSalt);
    real_input.clear();
    try {
      static_cast<void>(provider.mount_file(
          container_b.native(), first_password,
          {.drive_letter = first_drive,
           .prf = usb_vault::volume::Pkcs5Prf::sha512,
           .pim = 0,
           .read_only = false,
           .notify_shell = false}));
      first_mounted = true;
      first_password.clear();
      std::cout << "container B real-test mount passed\n";
    } catch (const std::system_error& error) {
      first_password.clear();
      card_secret.clear();
      report_system_error("container B real-test mount", error);
      return 12;
    }
    unmount_if_needed(provider, first_drive, first_mounted);

    usb_vault::security::SecureBuffer fixed_input = container_a_input();
    usb_vault::security::SecureBuffer second_password =
        usb_vault::security::derive_volume_password_v1(
            fixed_input, card_secret,
            usb_vault::application::kContainerASalt);
    fixed_input.clear();
    try {
      static_cast<void>(provider.mount_file(
          container_a.native(), second_password,
          {.drive_letter = second_drive,
           .prf = usb_vault::volume::Pkcs5Prf::sha512,
           .pim = 0,
           .read_only = false,
           .notify_shell = false}));
      second_mounted = true;
      second_password.clear();
      std::cout << "container A fixed mount passed\n";
    } catch (const std::system_error& error) {
      second_password.clear();
      card_secret.clear();
      report_system_error("container A fixed mount", error);
      return 13;
    }
    unmount_if_needed(provider, second_drive, second_mounted);
    std::cout << "P1.7 real-test individual mount diagnostic passed\n";

    usb_vault::security::SecureBuffer transaction_input = secure_text(
        usb_vault::development::kP17RealTestPassword);
    const usb_vault::application::ContainerLayout layout{
        .container_a = {
            .path = container_a.native(),
            .salt = usb_vault::application::kContainerASalt,
            .drive_letter = first_drive},
        .container_b = {
            .path = container_b.native(),
            .salt = usb_vault::application::kContainerBSalt,
            .drive_letter = second_drive},
    };
    bool transaction_a_mounted = false;
    bool transaction_b_mounted = false;
    try {
      const auto result = usb_vault::application::mount_session(
          provider, layout, transaction_input, card_secret);
      transaction_a_mounted = result.container_a.has_value();
      transaction_b_mounted = result.container_b.has_value();
      transaction_input.clear();
      unmount_if_needed(provider, second_drive, transaction_b_mounted, true);
      unmount_if_needed(provider, first_drive, transaction_a_mounted, true);
    } catch (const std::system_error& error) {
      transaction_input.clear();
      card_secret.clear();
      try {
        unmount_if_needed(provider, second_drive, transaction_b_mounted,
                          true);
        unmount_if_needed(provider, first_drive, transaction_a_mounted, true);
      } catch (...) {
        std::cerr << "two-container transaction cleanup failed\n";
        return 15;
      }
      report_system_error("two-container transaction", error);
      return 14;
    }
    card_secret.clear();
    std::cout << "P1.7 real-test two-container transaction passed\n";
    return 0;
  } catch (const std::system_error& error) {
    try {
      unmount_if_needed(provider, second_drive, second_mounted);
      unmount_if_needed(provider, first_drive, first_mounted);
    } catch (...) {
      std::cerr << "diagnostic cleanup failed\n";
      return 21;
    }
    report_system_error("diagnostic", error);
    return 20;
  } catch (const std::exception& error) {
    try {
      unmount_if_needed(provider, second_drive, second_mounted);
      unmount_if_needed(provider, first_drive, first_mounted);
    } catch (...) {
      std::cerr << "diagnostic cleanup failed\n";
      return 21;
    }
    std::cerr << "diagnostic failed: " << error.what() << '\n';
    return 20;
  }
}
