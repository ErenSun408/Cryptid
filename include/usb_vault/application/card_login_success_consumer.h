#pragma once

#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::application {

// Synchronous, non-UI handoff for one successfully authenticated Card Record.
// Implementations may move the root into the volatile authentication session
// and use the login input for immediate volume derivation. The login input must
// not be retained after this call returns or throws.
class ICardLoginSuccessConsumer {
 public:
  virtual ~ICardLoginSuccessConsumer() noexcept = default;

  ICardLoginSuccessConsumer(const ICardLoginSuccessConsumer&) = delete;
  ICardLoginSuccessConsumer& operator=(
      const ICardLoginSuccessConsumer&) = delete;
  ICardLoginSuccessConsumer(ICardLoginSuccessConsumer&&) = delete;
  ICardLoginSuccessConsumer& operator=(ICardLoginSuccessConsumer&&) = delete;

  virtual void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8) = 0;

 protected:
  ICardLoginSuccessConsumer() noexcept = default;
};

}  // namespace usb_vault::application
