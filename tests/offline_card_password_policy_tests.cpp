#include "usb_vault/security/offline_card_password_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using usb_vault::security::OfflineCardPasswordPolicy;
using usb_vault::security::PasswordStrength;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_bytes(
    const std::span<const std::byte> bytes) {
  SecureBuffer result(bytes.size());
  std::ranges::copy(bytes, result.bytes().begin());
  return result;
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  return secure_bytes(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] SecureBuffer repeated_ascii(const std::size_t size,
                                          const char value = 'a') {
  return secure_text(std::string(size, value));
}

void test_ascii_boundaries_and_feedback() {
  const OfflineCardPasswordPolicy policy;
  require(policy.assess(repeated_ascii(11)) == PasswordStrength::rejected,
          "11 code points met the minimum");
  require(policy.assess(repeated_ascii(12)) == PasswordStrength::acceptable,
          "12 code points did not meet the minimum");
  require(policy.assess(repeated_ascii(17)) == PasswordStrength::acceptable,
          "17 code points received the wrong feedback");
  require(policy.assess(repeated_ascii(18)) == PasswordStrength::strong,
          "18 code points did not receive strong feedback");
  require(policy.assess(repeated_ascii(128)) == PasswordStrength::strong,
          "128 code points exceeded the maximum");
  require(policy.assess(repeated_ascii(129)) == PasswordStrength::rejected,
          "129 code points did not exceed the maximum");
}

void test_unicode_is_counted_by_scalar_value() {
  const OfflineCardPasswordPolicy policy;
  const std::string chinese_12 = "甲乙丙丁戊己庚辛壬癸子丑";
  require(policy.assess(secure_text(chinese_12)) ==
              PasswordStrength::acceptable,
          "12 Chinese code points were counted as UTF-8 bytes");

  constexpr std::array<std::byte, 48> emoji_12 = {
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90}};
  require(policy.assess(secure_bytes(emoji_12)) ==
              PasswordStrength::acceptable,
          "12 supplementary code points were counted as UTF-8 bytes");
}

void test_composition_is_not_required() {
  const OfflineCardPasswordPolicy policy;
  require(policy.assess(secure_text("correct horse battery staple")) ==
              PasswordStrength::strong,
          "spaces or lower-case-only passphrase was rejected");
  require(policy.assess(secure_text("mossrivercedar")) ==
              PasswordStrength::acceptable,
          "single character class was rejected despite sufficient length");
}

void test_malformed_utf8_fails_closed() {
  const OfflineCardPasswordPolicy policy;
  constexpr std::array malformed_values{
      std::array{std::byte{0xC0}, std::byte{0xAF}, std::byte{0},
                 std::byte{0}},
      std::array{std::byte{0xE5}, std::byte{0xAF}, std::byte{0},
                 std::byte{0}},
      std::array{std::byte{0xED}, std::byte{0xA0}, std::byte{0x80},
                 std::byte{0}},
      std::array{std::byte{0xF4}, std::byte{0x90}, std::byte{0x80},
                 std::byte{0x80}}};
  constexpr std::array<std::size_t, 4> sizes{2, 2, 3, 4};

  for (std::size_t index = 0; index < malformed_values.size(); ++index) {
    require(policy.assess(secure_bytes(std::span(
                malformed_values[index].data(), sizes[index]))) ==
                PasswordStrength::rejected,
            "malformed UTF-8 was accepted");
  }
}

}  // namespace

int main() {
  try {
    test_ascii_boundaries_and_feedback();
    test_unicode_is_counted_by_scalar_value();
    test_composition_is_not_required();
    test_malformed_utf8_fails_closed();
    std::cout << "offline_card_password_policy_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "offline_card_password_policy_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
