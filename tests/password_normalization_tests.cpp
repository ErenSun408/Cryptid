#include "usb_vault/security/password_normalization.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using usb_vault::security::SecureBuffer;
using usb_vault::security::kMaximumPasswordUtf8InputBytes;
using usb_vault::security::normalize_password_utf8_nfc;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_bytes(
    const std::span<const std::byte> bytes) {
  SecureBuffer result(bytes.size());
  std::ranges::copy(bytes, result.bytes().begin());
  return result;
}

template <std::size_t Size>
void require_bytes(const SecureBuffer& actual,
                   const std::array<std::byte, Size>& expected,
                   const char* const message) {
  require(actual.size() == expected.size(), message);
  require(std::ranges::equal(actual.bytes(), expected), message);
}

void test_ascii_is_unchanged_and_input_is_consumed() {
  constexpr std::array input{
      std::byte{'p'}, std::byte{'a'}, std::byte{'s'}, std::byte{'s'}};
  SecureBuffer source = secure_bytes(input);
  SecureBuffer normalized = normalize_password_utf8_nfc(std::move(source));

  require(source.empty(), "normalization did not consume its input owner");
  require_bytes(normalized, input, "NFC changed ASCII password bytes");
}

void test_canonically_equivalent_sequences_converge() {
  constexpr std::array decomposed{
      std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}};
  constexpr std::array composed{std::byte{0xC3}, std::byte{0xA9}};

  SecureBuffer first =
      normalize_password_utf8_nfc(secure_bytes(decomposed));
  SecureBuffer second =
      normalize_password_utf8_nfc(secure_bytes(composed));
  require_bytes(first, composed, "decomposed accent did not become NFC");
  require(std::ranges::equal(first.bytes(), second.bytes()),
          "equivalent accent forms produced different NFC bytes");

  constexpr std::array hangul_jamo{
      std::byte{0xE1}, std::byte{0x84}, std::byte{0x80},
      std::byte{0xE1}, std::byte{0x85}, std::byte{0xA1}};
  constexpr std::array hangul_syllable{
      std::byte{0xEA}, std::byte{0xB0}, std::byte{0x80}};
  SecureBuffer hangul =
      normalize_password_utf8_nfc(secure_bytes(hangul_jamo));
  require_bytes(hangul, hangul_syllable,
                "Hangul Jamo did not compose to NFC");
}

void test_explicit_lengths_preserve_embedded_nul() {
  constexpr std::array input{
      std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}, std::byte{0},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90}};
  constexpr std::array expected{
      std::byte{0xC3}, std::byte{0xA9}, std::byte{0}, std::byte{0xF0},
      std::byte{0x9F}, std::byte{0x94}, std::byte{0x90}};
  SecureBuffer normalized =
      normalize_password_utf8_nfc(secure_bytes(input));
  require_bytes(normalized, expected,
                "NFC truncated or changed bytes around embedded NUL");
}

template <typename Exception, std::size_t Size>
void require_rejected(const std::array<std::byte, Size>& input,
                      const char* const message) {
  try {
    static_cast<void>(normalize_password_utf8_nfc(secure_bytes(input)));
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

void test_invalid_and_empty_input_fail_closed() {
  constexpr std::array overlong{std::byte{0xC0}, std::byte{0xAF}};
  constexpr std::array truncated{std::byte{0xE5}, std::byte{0xAF}};
  constexpr std::array surrogate{
      std::byte{0xED}, std::byte{0xA0}, std::byte{0x80}};
  require_rejected<std::invalid_argument>(overlong,
                                          "overlong UTF-8 was normalized");
  require_rejected<std::invalid_argument>(truncated,
                                          "truncated UTF-8 was normalized");
  require_rejected<std::invalid_argument>(surrogate,
                                          "UTF-8 surrogate was normalized");

  try {
    static_cast<void>(normalize_password_utf8_nfc(SecureBuffer{}));
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("empty password was normalized");
}

void test_input_byte_limit_precedes_conversion() {
  SecureBuffer oversized(kMaximumPasswordUtf8InputBytes + 1);
  std::ranges::fill(oversized.bytes(), std::byte{'a'});
  try {
    static_cast<void>(normalize_password_utf8_nfc(std::move(oversized)));
  } catch (const std::length_error&) {
    require(oversized.empty(), "oversized input owner was not consumed");
    return;
  }
  throw std::runtime_error("oversized password input was normalized");
}

}  // namespace

int main() {
  try {
    test_ascii_is_unchanged_and_input_is_consumed();
    test_canonically_equivalent_sequences_converge();
    test_explicit_lengths_preserve_embedded_nul();
    test_invalid_and_empty_input_fail_closed();
    test_input_byte_limit_precedes_conversion();
    std::cout << "password_normalization_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "password_normalization_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
