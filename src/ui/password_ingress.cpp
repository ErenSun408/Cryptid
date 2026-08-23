#include "usb_vault/ui/password_ingress.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>

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

void validate_utf8(const security::SecureBuffer& password_utf8) {
  if (password_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }
  if (password_utf8.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("password is too long for UTF-8 validation");
  }

  const auto* const password =
      reinterpret_cast<const char*>(password_utf8.data());
  ::SetLastError(ERROR_SUCCESS);
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password,
                            static_cast<int>(password_utf8.size()), nullptr,
                            0) != 0) {
    return;
  }

  const DWORD error = ::GetLastError();
  if (error == ERROR_NO_UNICODE_TRANSLATION) {
    throw std::invalid_argument("password must be well-formed UTF-8");
  }
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          "validate UI password UTF-8");
}

}  // namespace

security::SecureBuffer consume_password_utf8(
    const std::span<std::byte> mutable_utf8) {
  BridgeInputWiper input_wiper(mutable_utf8);
  if (mutable_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }
  if (mutable_utf8.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("password is too long for UTF-8 validation");
  }

  security::SecureBuffer password(mutable_utf8.size());
  std::ranges::copy(mutable_utf8, password.bytes().begin());
  input_wiper.wipe();
  validate_utf8(password);
  return password;
}

}  // namespace usb_vault::ui
