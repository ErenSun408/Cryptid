#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include "volume/locked_driver_buffer.h"
#include "volume/mount_request.h"
#include "volume/mount_identity.h"
#include "volume/veracrypt_driver_abi.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::security::SecureBuffer;
using usb_vault::volume::HiddenVolumeProtection;
using usb_vault::volume::MountOptions;
using usb_vault::volume::Pkcs5Prf;
using usb_vault::volume::detail::LockedDriverBuffer;
using usb_vault::volume::detail::abi::MountRequest;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Function>
void require_invalid_argument(Function&& function,
                              const char* const message) {
  try {
    function();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

SecureBuffer fixture_password(const std::size_t size,
                              const unsigned int seed = 1U) {
  SecureBuffer password(size);
  for (std::size_t index = 0; index < size; ++index) {
    password.bytes()[index] = static_cast<std::byte>((index + seed) & 0xffU);
  }
  return password;
}

void test_valid_mount_request() {
  LockedDriverBuffer<MountRequest> locked_request;
  MountRequest& request = locked_request.get();
  const SecureBuffer password = fixture_password(64);
  const MountOptions options{
      .drive_letter = L'w',
      .prf = Pkcs5Prf::sha512,
      .pim = 0,
      .read_only = false,
  };

  usb_vault::volume::detail::populate_mount_request(
      request, L"D:/vault/single.hc", password, options);

  require(request.drive_number == 22, "drive letter was not normalized");
  require(request.volume_path[2] == L'\\',
          "Windows path separators were not normalized for the driver");
  require(request.pkcs5_prf == 1, "SHA-512 PRF was not explicit");
  require(request.volume_password.length == 64,
          "password length was not preserved");
  for (std::size_t index = 0; index < password.size(); ++index) {
    require(request.volume_password.text[index] ==
                std::to_integer<std::uint8_t>(password.bytes()[index]),
            "password bytes were not preserved");
  }
  require(request.volume_password.text[64] == 0,
          "password terminator byte was not zero");
  require(request.cache_password == 0, "driver password caching was enabled");
  require(request.cache_pim == 0, "driver PIM caching was enabled");
  require(request.protect_hidden_volume == 0,
          "single-container mount enabled hidden-volume protection");
  require(request.mount_manager == 1, "mount manager was not enabled");
  require(request.preserve_timestamp == 1,
          "container timestamp preservation was not enabled");
  require(request.bytes_per_sector == 512,
          "logical sector size was not locked to the validated value");
  require(request.bytes_per_physical_sector == 512,
          "physical sector size was not locked to the validated value");
}

void test_invalid_mount_requests() {
  const MountOptions valid_options{.drive_letter = L'W'};
  const SecureBuffer valid_password = fixture_password(32);

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"relative.hc", valid_password, valid_options);
      },
      "relative path was accepted");

  require_invalid_argument(
      [&] {
        std::wstring embedded_null = L"D:\\vault\\one.hc";
        embedded_null.push_back(L'\0');
        embedded_null.append(L"other.hc");
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), embedded_null, valid_password, valid_options);
      },
      "embedded null path was accepted");

  require_invalid_argument(
      [&] {
        std::wstring too_long = L"D:\\";
        too_long.append(usb_vault::volume::detail::abi::kMaxPath, L'x');
        LockedDriverBuffer<MountRequest> request;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), too_long, valid_password, valid_options);
      },
      "overlong path was silently truncated");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer empty_password;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", empty_password,
            valid_options);
      },
      "empty password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer too_long_password = fixture_password(129);
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", too_long_password,
            valid_options);
      },
      "overlong password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.drive_letter = L'1';
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "invalid drive letter was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.prf = static_cast<Pkcs5Prf>(0);
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "automatic PRF probing was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.pim = usb_vault::volume::detail::abi::kMaxPimValue + 1;
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "out-of-range PIM was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "protection without a password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer empty_password;
        MountOptions options = valid_options;
        options.hidden_volume_protection =
            HiddenVolumeProtection{.password = &empty_password};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "empty protection password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        const SecureBuffer too_long_password = fixture_password(129);
        MountOptions options = valid_options;
        options.hidden_volume_protection =
            HiddenVolumeProtection{.password = &too_long_password};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "overlong protection password was accepted");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{
            .password = &valid_password, .prf = static_cast<Pkcs5Prf>(0)};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "automatic PRF probing was accepted for the protected hidden volume");

  require_invalid_argument(
      [&] {
        LockedDriverBuffer<MountRequest> request;
        MountOptions options = valid_options;
        options.hidden_volume_protection = HiddenVolumeProtection{
            .password = &valid_password,
            .pim = usb_vault::volume::detail::abi::kMaxPimValue + 1};
        usb_vault::volume::detail::populate_mount_request(
            request.get(), L"D:\\vault\\one.hc", valid_password, options);
      },
      "out-of-range protection PIM was accepted");
}

void test_protected_mount_request() {
  const SecureBuffer outer_password = fixture_password(48);
  const SecureBuffer hidden_password = fixture_password(72, 0x40U);

  LockedDriverBuffer<MountRequest> locked_request;
  MountRequest& request = locked_request.get();
  const MountOptions options{
      .drive_letter = L'Z',
      .prf = Pkcs5Prf::sha512,
      .pim = 0,
      .read_only = false,
      .notify_shell = false,
      .hidden_volume_protection =
          HiddenVolumeProtection{.password = &hidden_password},
  };

  usb_vault::volume::detail::populate_mount_request(
      request, L"D:\\vault\\pair.hc", outer_password, options);

  require(request.protect_hidden_volume == 1,
          "a maintenance mount left hidden-volume protection off");
  require(request.protected_hidden_volume_password.length == 72,
          "protection password length was not preserved");
  for (std::size_t index = 0; index < hidden_password.size(); ++index) {
    require(request.protected_hidden_volume_password.text[index] ==
                std::to_integer<std::uint8_t>(hidden_password.bytes()[index]),
            "protection password bytes were not preserved");
  }
  require(request.protected_hidden_volume_password.text[72] == 0,
          "protection password terminator byte was not zero");
  require(request.protected_hidden_volume_pkcs5_prf == 1,
          "SHA-512 PRF was not explicit for the protected hidden volume");

  // A request that swapped the slots would mount the wrong volume or protect
  // nothing, and the driver refuses both the same way.
  require(request.volume_password.length == 48,
          "the mounted volume password was displaced by the protection one");
  for (std::size_t index = 0; index < outer_password.size(); ++index) {
    require(request.volume_password.text[index] ==
                std::to_integer<std::uint8_t>(outer_password.bytes()[index]),
            "the mounted volume password bytes were not preserved");
  }

  // The product always uses PIM 0; this pins which field each one reaches,
  // since they sit 4 bytes apart.
  LockedDriverBuffer<MountRequest> locked_pim_request;
  MountRequest& pim_request = locked_pim_request.get();
  MountOptions pim_options = options;
  pim_options.hidden_volume_protection =
      HiddenVolumeProtection{.password = &hidden_password, .pim = 485};

  usb_vault::volume::detail::populate_mount_request(
      pim_request, L"D:\\vault\\pair.hc", outer_password, pim_options);

  require(pim_request.protected_hidden_volume_pim == 485,
          "protection PIM did not reach the protected hidden volume");
  require(pim_request.volume_pim == 0,
          "protection PIM displaced the mounted volume PIM");
}

void test_mount_identity_validation() {
  using usb_vault::volume::detail::decode_mount_identity;
  usb_vault::volume::detail::abi::VolumeProperties p{};
  p.drive_number = 24;
  p.unique_id = 0; // The driver's first mount is numbered zero.
  p.volume_path[0] = L'D';
  p.volume_path[1] = L':';
  p.disk_length = 1024;
  p.pkcs5_prf = 1;
  p.hidden_protection = 1;
  const auto valid = decode_mount_identity(p, 24);
  require(valid.protected_outer_writable && valid.unique_id == 0 &&
          valid.drive_letter == L'Y', "valid protected mount identity rejected");
  auto bad = p;
  bad.unique_id = -1;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "negative identity accepted");
  require_invalid_argument([&]{decode_mount_identity(p, 23);}, "another drive accepted");
  bad = p; bad.disk_length = 0;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "empty volume accepted");
  bad = p; std::fill(std::begin(bad.volume_path), std::end(bad.volume_path), L'x');
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "unterminated path accepted");
  bad = p; bad.hidden_protection = 3;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "unknown protection accepted");
  bad = p; bad.read_only = 2;
  require_invalid_argument([&]{decode_mount_identity(bad, 24);}, "malformed boolean accepted");
  for (const auto field : {&decltype(p)::hidden_volume, &decltype(p)::read_only,
       &decltype(p)::partition_in_inactive_system_scope, &decltype(p)::mount_disabled,
       &decltype(p)::volume_pim}) {
    bad = p; bad.*field = 1;
    require(!decode_mount_identity(bad, 24).protected_outer_writable,
            "unsupported mount marked writable protected outer");
  }
  for (const int protection : {0, 2}) {
    bad = p; bad.hidden_protection = protection;
    require(!decode_mount_identity(bad, 24).protected_outer_writable,
            "unprotected or triggered volume accepted");
  }
  bad = p; bad.unique_id = 1;
  require(decode_mount_identity(bad, 24) != valid, "remount identity was ignored");
  bad = p; bad.header_flags = 1;
  require(!decode_mount_identity(bad, 24).protected_outer_writable, "header flags ignored");
  bad = p; bad.pkcs5_prf = 2;
  require(!decode_mount_identity(bad, 24).protected_outer_writable, "unsupported PRF ignored");
}

void test_driver_error_codes() {
  const std::error_code busy =
      usb_vault::volume::make_error_code(
          usb_vault::volume::DriverError::volume_busy);
  require(busy.value() == 6, "busy driver error value changed");
  require(busy.category() == usb_vault::volume::driver_error_category(),
          "busy driver error category changed");
  require(busy.message() == "volume still has open files",
          "busy driver error message changed");
}

}  // namespace

int main() {
  try {
    test_valid_mount_request();
    test_protected_mount_request();
    test_invalid_mount_requests();
    test_driver_error_codes();
    test_mount_identity_validation();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }

  return 0;
}
