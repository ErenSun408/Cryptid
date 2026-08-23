#include "usb_vault/ui/password_ingress.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

using usb_vault::security::SecureBuffer;
using usb_vault::ui::consume_password_utf8;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <std::size_t Size>
void require_wiped(const std::array<std::byte, Size>& input) {
  for (const std::byte value : input) {
    require(value == std::byte{0}, "UI bridge input was not wiped");
  }
}

template <std::size_t Size>
void require_password(const SecureBuffer& password,
                      const std::array<std::byte, Size>& expected) {
  require(password.size() == expected.size(), "password size differed");
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(password.bytes()[index] == expected[index],
            "password bytes differed");
  }
}

template <typename Exception, std::size_t Size>
void require_rejected_and_wiped(std::array<std::byte, Size>& input,
                                const char* const message) {
  try {
    static_cast<void>(consume_password_utf8(input));
  } catch (const Exception&) {
    require_wiped(input);
    return;
  }
  throw std::runtime_error(message);
}

void test_ascii_is_copied_then_source_is_wiped() {
  constexpr std::array expected{
      std::byte{'p'}, std::byte{'a'}, std::byte{'s'}, std::byte{'s'}};
  auto input = expected;

  SecureBuffer password = consume_password_utf8(input);

  require_password(password, expected);
  require_wiped(input);
}

void test_non_ascii_and_embedded_nul_are_preserved() {
  constexpr std::array expected{
      std::byte{0xE5}, std::byte{0xAF}, std::byte{0x86}, std::byte{0x00},
      std::byte{0xF0}, std::byte{0x9F}, std::byte{0x94}, std::byte{0x90}};
  auto input = expected;

  SecureBuffer password = consume_password_utf8(input);

  require_password(password, expected);
  require_wiped(input);
}

void test_invalid_sequences_are_rejected_and_wiped() {
  std::array overlong{std::byte{0xC0}, std::byte{0xAF}};
  require_rejected_and_wiped<std::invalid_argument>(
      overlong, "overlong UTF-8 was accepted");

  std::array truncated{std::byte{0xE5}, std::byte{0xAF}};
  require_rejected_and_wiped<std::invalid_argument>(
      truncated, "truncated UTF-8 was accepted");

  std::array surrogate{std::byte{0xED}, std::byte{0xA0}, std::byte{0x80}};
  require_rejected_and_wiped<std::invalid_argument>(
      surrogate, "UTF-8 surrogate encoding was accepted");
}

void test_empty_input_is_rejected() {
  std::span<std::byte> empty;
  try {
    static_cast<void>(consume_password_utf8(empty));
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("empty UI password was accepted");
}

}  // namespace

int main() {
  test_ascii_is_copied_then_source_is_wiped();
  test_non_ascii_and_embedded_nul_are_preserved();
  test_invalid_sequences_are_rejected_and_wiped();
  test_empty_input_is_rejected();

  std::cout << "password_ingress_tests: PASS\n";
  return 0;
}
