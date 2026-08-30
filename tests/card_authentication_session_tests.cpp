#include "usb_vault/application/card_authentication_session.h"

#include "usb_vault/card/card_record_v1.h"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

using usb_vault::application::CardAuthenticationSession;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer root_secret(
    const std::size_t size =
        usb_vault::card::kCardRecordV1RootSecretSize) {
  SecureBuffer result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

void test_starts_unauthenticated_and_establishes_by_move() {
  CardAuthenticationSession session;
  require(!session.authenticated(),
          "authentication session started active");

  SecureBuffer root = root_secret();
  session.establish(std::move(root));
  require(root.empty(), "authentication session copied its root input");
  require(session.authenticated(),
          "valid root did not establish authentication");
}

void test_invalid_root_sizes_are_rejected_without_state() {
  for (const std::size_t size : {
           std::size_t{0},
           usb_vault::card::kCardRecordV1RootSecretSize - 1,
           usb_vault::card::kCardRecordV1RootSecretSize + 1,
       }) {
    CardAuthenticationSession session;
    SecureBuffer invalid = root_secret(size);
    try {
      session.establish(std::move(invalid));
      throw std::runtime_error("invalid root size was accepted");
    } catch (const std::invalid_argument&) {
    }
    require(invalid.empty() && !session.authenticated(),
            "invalid root was copied or established state");
  }
}

void test_active_session_cannot_be_replaced() {
  CardAuthenticationSession session;
  session.establish(root_secret());
  SecureBuffer replacement = root_secret();

  try {
    session.establish(std::move(replacement));
    throw std::runtime_error("active authentication was replaced");
  } catch (const std::logic_error&) {
  }
  require(replacement.empty() && session.authenticated(),
          "replacement failure lost or copied authentication state");
}

void test_clear_is_immediate_and_idempotent() {
  CardAuthenticationSession session;
  session.establish(root_secret());

  session.clear_authentication();
  require(!session.authenticated(),
          "clear retained authenticated state");
  session.clear_authentication();
  require(!session.authenticated(),
          "duplicate clear changed authentication state");

  session.establish(root_secret());
  require(session.authenticated(),
          "fresh root was rejected after clear");
}

}  // namespace

int main() {
  try {
    test_starts_unauthenticated_and_establishes_by_move();
    test_invalid_root_sizes_are_rejected_without_state();
    test_active_session_cannot_be_replaced();
    test_clear_is_immediate_and_idempotent();
    std::cout << "card_authentication_session_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_authentication_session_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
