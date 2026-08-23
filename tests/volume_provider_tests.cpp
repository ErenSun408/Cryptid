#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include "volume/locked_driver_buffer.h"
#include "volume/mount_request.h"
#include "volume/veracrypt_driver_abi.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::security::SecureBuffer;
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

SecureBuffer fixture_password(const std::size_t size) {
  SecureBuffer password(size);
  for (std::size_t index = 0; index < size; ++index) {
    password.bytes()[index] = static_cast<std::byte>((index + 1U) & 0xffU);
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
    test_invalid_mount_requests();
    test_driver_error_codes();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }

  return 0;
}
