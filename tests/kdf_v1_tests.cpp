#include "usb_vault/security/kdf_v1.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::security::KdfV1Salt;
using usb_vault::security::SecureBuffer;
using usb_vault::security::derive_volume_password_v1;

[[noreturn]] void fail(const char* const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char* const message) {
  if (!condition) {
    fail(message);
  }
}

template <std::size_t Size>
SecureBuffer secure_copy(const std::array<std::byte, Size>& source) {
  SecureBuffer destination(source.size());
  std::ranges::copy(source, destination.bytes().begin());
  return destination;
}

template <typename Action>
void expect_invalid_argument(Action&& action, const char* const message) {
  try {
    action();
  } catch (const std::invalid_argument&) {
    return;
  }
  fail(message);
}

void expect_locked(SecureBuffer& buffer, const char* const message) {
  PSAPI_WORKING_SET_EX_INFORMATION information{};
  information.VirtualAddress = buffer.data();
  const BOOL queried = ::QueryWorkingSetEx(
      ::GetCurrentProcess(), &information, sizeof(information));
  expect(queried != FALSE, "QueryWorkingSetEx must succeed");
  expect(information.VirtualAttributes.Valid != 0,
         "derived password page must be resident");
  expect(information.VirtualAttributes.Locked != 0, message);
}

void test_rejects_empty_or_invalid_utf8_before_derivation() {
  SecureBuffer empty_password;
  SecureBuffer card_secret(usb_vault::security::kKdfV1CardSecretSize);
  KdfV1Salt salt{};

  expect_invalid_argument(
      [&] {
        static_cast<void>(
            derive_volume_password_v1(empty_password, card_secret, salt));
      },
      "empty password must be rejected");

  const std::array invalid_utf8{std::byte{0xC0}, std::byte{0xAF}};
  SecureBuffer invalid_password = secure_copy(invalid_utf8);
  expect_invalid_argument(
      [&] {
        static_cast<void>(
            derive_volume_password_v1(invalid_password, card_secret, salt));
      },
      "overlong UTF-8 sequence must be rejected");
}

void test_rejects_wrong_card_secret_size_before_derivation() {
  const std::array password{std::byte{'x'}};
  SecureBuffer password_utf8 = secure_copy(password);
  SecureBuffer short_card_secret(
      usb_vault::security::kKdfV1CardSecretSize - 1);
  KdfV1Salt salt{};

  expect_invalid_argument(
      [&] {
        static_cast<void>(derive_volume_password_v1(
            password_utf8, short_card_secret, salt));
      },
      "short card secret must be rejected");
}

void test_matches_independent_production_parameter_vector() {
  // UTF-8 for "pässwörd-密码", supplied as exact bytes so no source or runtime
  // normalization can affect the vector.
  constexpr std::array password{
      std::byte{0x70}, std::byte{0xC3}, std::byte{0xA4}, std::byte{0x73},
      std::byte{0x73}, std::byte{0x77}, std::byte{0xC3}, std::byte{0xB6},
      std::byte{0x72}, std::byte{0x64}, std::byte{0x2D}, std::byte{0xE5},
      std::byte{0xAF}, std::byte{0x86}, std::byte{0xE7}, std::byte{0xA0},
      std::byte{0x81}};
  constexpr std::array card_secret{
      std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
      std::byte{0x04}, std::byte{0x05}, std::byte{0x06}, std::byte{0x07},
      std::byte{0x08}, std::byte{0x09}, std::byte{0x0A}, std::byte{0x0B},
      std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
      std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
      std::byte{0x18}, std::byte{0x19}, std::byte{0x1A}, std::byte{0x1B},
      std::byte{0x1C}, std::byte{0x1D}, std::byte{0x1E}, std::byte{0x1F}};
  constexpr KdfV1Salt salt{
      std::byte{0xF0}, std::byte{0xF1}, std::byte{0xF2}, std::byte{0xF3},
      std::byte{0xF4}, std::byte{0xF5}, std::byte{0xF6}, std::byte{0xF7},
      std::byte{0xF8}, std::byte{0xF9}, std::byte{0xFA}, std::byte{0xFB},
      std::byte{0xFC}, std::byte{0xFD}, std::byte{0xFE}, std::byte{0xFF}};
  constexpr std::array expected{
      std::byte{0xAB}, std::byte{0x9C}, std::byte{0x82}, std::byte{0x02},
      std::byte{0x51}, std::byte{0x15}, std::byte{0xEB}, std::byte{0xFE},
      std::byte{0xA6}, std::byte{0xBA}, std::byte{0xE1}, std::byte{0x33},
      std::byte{0xD9}, std::byte{0xFC}, std::byte{0x73}, std::byte{0x48},
      std::byte{0xAE}, std::byte{0x11}, std::byte{0xCA}, std::byte{0xA6},
      std::byte{0x04}, std::byte{0x2A}, std::byte{0xBD}, std::byte{0xCD},
      std::byte{0x7E}, std::byte{0xD3}, std::byte{0x53}, std::byte{0xA8},
      std::byte{0xF6}, std::byte{0x98}, std::byte{0x16}, std::byte{0x7E},
      std::byte{0xD8}, std::byte{0x9C}, std::byte{0x0F}, std::byte{0x13},
      std::byte{0xF5}, std::byte{0xB4}, std::byte{0x9A}, std::byte{0xF0},
      std::byte{0x0C}, std::byte{0xCB}, std::byte{0x8C}, std::byte{0xA7},
      std::byte{0x02}, std::byte{0x0D}, std::byte{0xD9}, std::byte{0x61},
      std::byte{0x34}, std::byte{0x2D}, std::byte{0x4E}, std::byte{0x65},
      std::byte{0xDA}, std::byte{0x1F}, std::byte{0x39}, std::byte{0x38},
      std::byte{0x61}, std::byte{0x35}, std::byte{0xE3}, std::byte{0x74},
      std::byte{0xB1}, std::byte{0x12}, std::byte{0x74}, std::byte{0x72}};

  SecureBuffer password_utf8 = secure_copy(password);
  SecureBuffer card = secure_copy(card_secret);
  const auto started = std::chrono::steady_clock::now();
  SecureBuffer output =
      derive_volume_password_v1(password_utf8, card, salt);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  expect(output.size() == expected.size(),
         "KDF output must contain exactly 64 bytes");
  expect(std::ranges::equal(output.bytes(), expected),
         "KDF output must match the independent cryptography vector");
  expect_locked(output, "derived password output must remain page-locked");

  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  std::cout << "kdf_v1 production vector elapsed_ms=" << elapsed_ms << '\n';
}

}  // namespace

int main() {
  test_rejects_empty_or_invalid_utf8_before_derivation();
  test_rejects_wrong_card_secret_size_before_derivation();
  test_matches_independent_production_parameter_vector();

  std::cout << "kdf_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
