#include "card/vikey/vikey_development_app_config.h"
#include "../src/platform/windows/vikey_configuration/checks.h"

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

void test_environment_never_calls_card() {
  using namespace configuration;
  for (unsigned int failed = 0; failed <= 5; ++failed) {
    Probes probes;
    unsigned int calls = 0;
    for (unsigned int index = 0; index < count; ++index) {
      probes[index] = [&, index] {
        expect(index < 5, "environment check accessed card");
        ++calls;
        return Result{index == failed ? State::failed : State::passed, L"test"};
      };
    }
    expect(run_checks(probes, [](unsigned int, const Result&) {}, false) == (failed == 5),
           "environment result ignored failure");
    expect(calls == 5, "environment check did not inspect all prerequisites");
  }
}

void test_diagnostic_gates() {
  using namespace configuration;
  for (const unsigned int failed : {1U, 4U, 5U, count}) {
    Probes probes;
    unsigned int calls = 0;
    bool card_called = false;
    for (unsigned int index = 0; index < count; ++index) {
      probes[index] = [&, index] {
        ++calls;
        if (index == 6) card_called = true;
        return Result{index == failed ? State::failed : State::passed, L"test"};
      };
    }
    State last{};
    const bool passed = run_checks(probes, [&](unsigned int index, const Result& result) {
      if (index == 6) last = result.state;
    });
    expect(passed == (failed == count), "diagnostic success ignored prerequisite failure");
    expect(card_called == passed && calls == (passed ? count : count - 1),
           "card probe ran without prerequisites");
    expect(last == (passed ? State::passed : State::skipped), "incorrect card result");
  }
  Probes probes;
  for (auto& probe : probes) probe = [] { return Result{State::passed, L"test"}; };
  probes[3] = []() -> Result { throw std::runtime_error("private diagnostic detail"); };
  bool sanitized = false;
  expect(!run_checks(probes, [&](unsigned int index, const Result& result) {
    if (index == 3 && result.state == State::failed)
      sanitized = result.detail.find(L"private") == std::wstring::npos;
  }) && sanitized, "diagnostic error was not sanitized");
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
    test_environment_never_calls_card();
    test_diagnostic_gates();
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
