#include "card/vikey/vikey_development_credentials.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using usb_vault::card::vikey::development::WindowsCredentialProvider;
using usb_vault::card::vikey::development::CredentialField;
using usb_vault::card::vikey::development::decode_credential_blob;
using usb_vault::card::vikey::development::valid_credential_value;

static_assert(!std::is_copy_constructible_v<WindowsCredentialProvider>);
static_assert(!std::is_copy_assignable_v<WindowsCredentialProvider>);

[[noreturn]] void fail(const char* const message) {
  throw std::runtime_error(message);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

template <typename Operation>
void expect_invalid(Operation&& operation, const char* const message) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  fail(message);
}

void expect_bytes(const usb_vault::security::SecureBuffer& value,
                  const std::array<std::byte, 4>& expected,
                  const char* const message) {
  expect(value.size() == expected.size(), message);
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (value.bytes()[index] != expected[index]) fail(message);
  }
}

void test_raw_layouts() {
  constexpr std::array raw{
      std::byte{'A'}, std::byte{'1'}, std::byte{'b'}, std::byte{'2'}};
  auto exact = decode_credential_blob(raw, raw.size());
  expect_bytes(exact, raw, "raw credential did not decode");

  constexpr std::array terminated{
      std::byte{'A'}, std::byte{'1'}, std::byte{'b'}, std::byte{'2'},
      std::byte{}};
  auto with_terminator = decode_credential_blob(terminated, raw.size());
  expect_bytes(with_terminator, raw,
               "terminated raw credential did not decode");
}

void test_utf16_layouts() {
  constexpr std::array expected{
      std::byte{'A'}, std::byte{'1'}, std::byte{'b'}, std::byte{'2'}};
  constexpr std::array utf16{
      std::byte{'A'}, std::byte{}, std::byte{'1'}, std::byte{},
      std::byte{'b'}, std::byte{}, std::byte{'2'}, std::byte{}};
  auto exact = decode_credential_blob(utf16, expected.size());
  expect_bytes(exact, expected, "UTF-16 credential did not decode");

  constexpr std::array terminated{
      std::byte{'A'}, std::byte{}, std::byte{'1'}, std::byte{},
      std::byte{'b'}, std::byte{}, std::byte{'2'}, std::byte{},
      std::byte{}, std::byte{}};
  auto with_terminator =
      decode_credential_blob(terminated, expected.size());
  expect_bytes(with_terminator, expected,
               "terminated UTF-16 credential did not decode");
}

void test_invalid_layouts() {
  constexpr std::array short_value{
      std::byte{'A'}, std::byte{'1'}, std::byte{'b'}};
  expect_invalid(
      [&] { static_cast<void>(decode_credential_blob(short_value, 4)); },
      "short credential was accepted");

  constexpr std::array non_ascii_utf16{
      std::byte{'A'}, std::byte{}, std::byte{'1'}, std::byte{1},
      std::byte{'b'}, std::byte{}, std::byte{'2'}, std::byte{}};
  expect_invalid(
      [&] {
        static_cast<void>(decode_credential_blob(non_ascii_utf16, 4));
      },
      "non-ASCII UTF-16 credential was accepted");

  expect_invalid(
      [&] {
        static_cast<void>(decode_credential_blob(
            std::span<const std::byte>{}, 0));
      },
      "zero expected credential size was accepted");
}

void test_write_shape_validation() {
  constexpr std::array user_sn{
      std::byte{'0'}, std::byte{'1'}, std::byte{'a'}, std::byte{'F'}};
  expect(valid_credential_value(CredentialField::user_sn, user_sn) ==
             false,
         "a short UserSN was accepted");

  std::array<std::byte, 32> valid_user_sn{};
  valid_user_sn.fill(std::byte{'a'});
  expect(valid_credential_value(CredentialField::user_sn, valid_user_sn),
         "a valid UserSN was rejected");
  valid_user_sn[7] = std::byte{'g'};
  expect(!valid_credential_value(CredentialField::user_sn, valid_user_sn),
         "a non-hexadecimal UserSN was accepted");

  std::array<std::byte, 128> valid_api_key{};
  valid_api_key.fill(std::byte{'9'});
  expect(valid_credential_value(CredentialField::api_key, valid_api_key),
         "a valid API key was rejected");
  valid_api_key[127] = std::byte{'Z'};
  expect(!valid_credential_value(CredentialField::api_key, valid_api_key),
         "a non-hexadecimal API key was accepted");

  constexpr std::array valid_pin{
      std::byte{'A'}, std::byte{'b'}, std::byte{'0'}, std::byte{'1'},
      std::byte{'x'}, std::byte{'Y'}, std::byte{'8'}, std::byte{'9'}};
  expect(valid_credential_value(CredentialField::user_pin, valid_pin),
         "a valid user PIN was rejected");
  auto invalid_pin = valid_pin;
  invalid_pin[2] = std::byte{'-'};
  expect(!valid_credential_value(CredentialField::user_pin, invalid_pin),
         "a non-alphanumeric user PIN was accepted");

  expect(!valid_credential_value(
             static_cast<CredentialField>(999), valid_pin),
         "an unknown credential field was accepted");
}

}  // namespace

int main() {
  try {
    test_raw_layouts();
    test_utf16_layouts();
    test_invalid_layouts();
    test_write_shape_validation();
    std::cout << "vikey_development_credentials_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_development_credentials_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
