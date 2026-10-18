#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstdint>

namespace usb_vault::application {

namespace detail {
class CardLoginMountSessionCore;
}
class CardContactBook;

// Minimal lifecycle boundary used by physical-card event coordination. It
// reveals no root bytes, password role, mounted shape, mode, or vendor data.
class IVolatileCardAuthenticationSession {
 public:
  virtual ~IVolatileCardAuthenticationSession() noexcept = default;

  IVolatileCardAuthenticationSession(
      const IVolatileCardAuthenticationSession&) = delete;
  IVolatileCardAuthenticationSession& operator=(
      const IVolatileCardAuthenticationSession&) = delete;
  IVolatileCardAuthenticationSession(
      IVolatileCardAuthenticationSession&&) = delete;
  IVolatileCardAuthenticationSession& operator=(
      IVolatileCardAuthenticationSession&&) = delete;

  virtual void clear_authentication() noexcept = 0;
  [[nodiscard]] virtual bool authenticated() const noexcept = 0;

 protected:
  IVolatileCardAuthenticationSession() noexcept = default;
};

// Owns one unlocked Card Record root secret only in page-locked process
// memory. Establishment consumes an exact v1 root and refuses to replace an
// active session. No byte accessor is exposed at this boundary; later mount
// and volume operations must receive a separately reviewed scoped consumer.
class CardAuthenticationSession final
    : public IVolatileCardAuthenticationSession {
 public:
  CardAuthenticationSession() noexcept = default;
  ~CardAuthenticationSession() noexcept override = default;

  CardAuthenticationSession(const CardAuthenticationSession&) = delete;
  CardAuthenticationSession& operator=(const CardAuthenticationSession&) =
      delete;
  CardAuthenticationSession(CardAuthenticationSession&&) = delete;
  CardAuthenticationSession& operator=(CardAuthenticationSession&&) = delete;

  void establish(security::SecureBuffer root_secret);
  void clear_authentication() noexcept override;
  [[nodiscard]] bool authenticated() const noexcept override;
  // Changes with every establishment, so state held for one login can tell
  // that it no longer belongs to the current one. Carries no secret.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

 private:
  friend class detail::CardLoginMountSessionCore;
  // Derives the contact identity and store key once per login (P5.14). Only
  // its constructor reads root_secret_; any other use needs its own review.
  friend class CardContactBook;

  security::SecureBuffer root_secret_;
  // Accessed only on the serialized native session worker. Never persisted.
  std::uint64_t generation_{};
};

}  // namespace usb_vault::application
