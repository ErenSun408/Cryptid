#include "usb_vault/card/card_error.h"

#include <string>

namespace usb_vault::card {
namespace {

class CardErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "usb-vault-card";
  }

  [[nodiscard]] std::string message(const int value) const override {
    switch (static_cast<CardError>(value)) {
      case CardError::backend_unavailable:
        return "card backend is unavailable";
      case CardError::invalid_configuration:
        return "card backend configuration is invalid";
      case CardError::device_not_found:
        return "card device was not found";
      case CardError::authentication_failed:
        return "card authentication failed";
      case CardError::authentication_locked:
        return "card authentication is locked";
      case CardError::permission_denied:
        return "card operation requires a different permission level";
      case CardError::unsupported:
        return "card operation is not supported";
      case CardError::communication_failed:
        return "card communication failed";
      case CardError::invalid_operation:
        return "card operation or data range is invalid";
      case CardError::device_failure:
        return "card device reported an internal failure";
      default:
        return "unknown card error";
    }
  }
};

}  // namespace

const std::error_category& card_error_category() noexcept {
  static CardErrorCategory category;
  return category;
}

std::error_code make_error_code(const CardError error) noexcept {
  return {static_cast<int>(error), card_error_category()};
}

}  // namespace usb_vault::card
