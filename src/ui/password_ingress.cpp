#include "usb_vault/ui/password_ingress.h"

#include "usb_vault/security/password_normalization.h"

#include <Windows.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace usb_vault::ui {
namespace {

class BridgeInputWiper final {
 public:
  explicit BridgeInputWiper(const std::span<std::byte> bytes) noexcept
      : bytes_(bytes) {}

  ~BridgeInputWiper() noexcept { wipe(); }

  BridgeInputWiper(const BridgeInputWiper&) = delete;
  BridgeInputWiper& operator=(const BridgeInputWiper&) = delete;

  void wipe() noexcept {
    if (!wiped_ && !bytes_.empty()) {
      static_cast<void>(::SecureZeroMemory(bytes_.data(), bytes_.size()));
    }
    wiped_ = true;
  }

 private:
  std::span<std::byte> bytes_;
  bool wiped_{false};
};

}  // namespace

security::SecureBuffer consume_password_utf8(
    const std::span<std::byte> mutable_utf8) {
  BridgeInputWiper input_wiper(mutable_utf8);
  if (mutable_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }
  if (mutable_utf8.size() >
      security::kMaximumPasswordUtf8InputBytes) {
    throw std::length_error("password UTF-8 input is too long");
  }

  security::SecureBuffer password(mutable_utf8.size());
  std::ranges::copy(mutable_utf8, password.bytes().begin());
  input_wiper.wipe();
  return security::normalize_password_utf8_nfc(std::move(password));
}

}  // namespace usb_vault::ui
