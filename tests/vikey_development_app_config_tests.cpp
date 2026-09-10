#include "card/vikey/vikey_development_app_config.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using usb_vault::card::vikey::development::
    kNetworkEnabledDevelopmentAcknowledgement;
using usb_vault::card::vikey::development::parse_app_config;

constexpr std::wstring_view kHelperDigest =
    L"000102030405060708090A0B0C0D0E0F"
    L"101112131415161718191A1B1C1D1E1F";
constexpr std::wstring_view kVendorDigest =
    L"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
    L"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";

void expect(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
void expect_invalid(Operation&& operation, const char* const message) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

void test_valid_config() {
  constexpr std::array arguments{
      std::wstring_view{L"C:\\vault\\usb_vault_vikey_development_app.exe"},
      kNetworkEnabledDevelopmentAcknowledgement,
      std::wstring_view{L"C:\\vault\\bin\\..\\helper.exe"},
      kHelperDigest,
      std::wstring_view{L"D:\\vendor\\vikey"},
      kVendorDigest,
  };
  const auto config = parse_app_config(arguments);
  expect(config.helper_path == L"C:\\vault\\helper.exe",
         "helper path was not normalized");
  expect(config.vendor_directory == L"D:\\vendor\\vikey",
         "vendor directory changed");
  for (std::size_t index = 0; index < config.helper_digest.size(); ++index) {
    expect(config.helper_digest[index] == static_cast<std::byte>(index),
           "helper digest decoded incorrectly");
    expect(config.vendor_digest[index] == std::byte{0xff},
           "vendor digest decoded incorrectly");
  }
}

void test_packaged_config() {
  using usb_vault::card::vikey::development::packaged_app_config;
  const auto config = packaged_app_config(L"C:\\测试 包\\app", kHelperDigest, kVendorDigest);
  expect(config.helper_path == L"C:\\测试 包\\app\\usb_vault_vikey_helper.exe",
         "packaged helper must be relative to executable directory");
  expect(config.vendor_directory == L"C:\\测试 包\\app\\vendor",
         "packaged vendor must stay beside executable");
  expect(config.helper_digest[31] == std::byte{31} &&
         config.vendor_digest[0] == std::byte{0xff}, "packaged digests changed");
  expect_invalid([&] { static_cast<void>(packaged_app_config(L"relative", kHelperDigest, kVendorDigest)); },
                 "packaged directory must be absolute");
  expect_invalid([&] { static_cast<void>(packaged_app_config(L"C:\\app", L"bad", kVendorDigest)); },
                 "invalid compiled digest accepted");
}

void test_acknowledgement_is_mandatory() {
  constexpr std::array arguments{
      std::wstring_view{L"C:\\vault\\app.exe"},
      std::wstring_view{L"--ordinary-mode"},
      std::wstring_view{L"C:\\vault\\helper.exe"},
      kHelperDigest,
      std::wstring_view{L"D:\\vendor\\vikey"},
      kVendorDigest,
  };
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(arguments)); },
      "missing network-development acknowledgement was accepted");
}

void test_paths_must_be_absolute() {
  constexpr std::array relative_helper{
      std::wstring_view{L"C:\\vault\\app.exe"},
      kNetworkEnabledDevelopmentAcknowledgement,
      std::wstring_view{L"helper.exe"},
      kHelperDigest,
      std::wstring_view{L"D:\\vendor\\vikey"},
      kVendorDigest,
  };
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(relative_helper)); },
      "relative helper path was accepted");

  auto relative_vendor = relative_helper;
  relative_vendor[2] = L"C:\\vault\\helper.exe";
  relative_vendor[4] = L"vendor";
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(relative_vendor)); },
      "relative vendor path was accepted");
}

void test_digests_and_argument_count_fail_closed() {
  std::array arguments{
      std::wstring_view{L"C:\\vault\\app.exe"},
      kNetworkEnabledDevelopmentAcknowledgement,
      std::wstring_view{L"C:\\vault\\helper.exe"},
      std::wstring_view{L"00"},
      std::wstring_view{L"D:\\vendor\\vikey"},
      kVendorDigest,
  };
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(arguments)); },
      "short helper digest was accepted");

  arguments[3] = kHelperDigest;
  arguments[5] =
      L"GFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(arguments)); },
      "non-hexadecimal vendor digest was accepted");

  constexpr std::array extra_arguments{
      std::wstring_view{L"C:\\vault\\app.exe"},
      kNetworkEnabledDevelopmentAcknowledgement,
      std::wstring_view{L"C:\\vault\\helper.exe"},
      kHelperDigest,
      std::wstring_view{L"D:\\vendor\\vikey"},
      kVendorDigest,
      std::wstring_view{L"unexpected"},
  };
  expect_invalid(
      [&] { static_cast<void>(parse_app_config(extra_arguments)); },
      "extra development argument was accepted");
}

}  // namespace

int main() {
  try {
    test_valid_config();
    test_packaged_config();
    test_acknowledgement_is_mandatory();
    test_paths_must_be_absolute();
    test_digests_and_argument_count_fail_closed();
    std::cout << "vikey_development_app_config_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_development_app_config_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
