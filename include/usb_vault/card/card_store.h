#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstdint>

namespace usb_vault::card {

enum class CardCapability : std::uint32_t {
  multiple_pins = 1U << 0,
};

struct CardCapabilities final {
  std::uint32_t bits{};

  [[nodiscard]] constexpr bool supports(
      const CardCapability capability) const noexcept {
    return (bits & static_cast<std::uint32_t>(capability)) != 0;
  }
};

// Provides the single opaque blob boundary required by the application layer.
// Implementations must return secrets in SecureBuffer and must not persist or
// classify the blob outside the card. Optional hardware behavior is exposed
// only through capability bits and does not change this contract.
class ICardStore {
 public:
  virtual ~ICardStore() noexcept = default;

  ICardStore(const ICardStore&) = delete;
  ICardStore& operator=(const ICardStore&) = delete;
  ICardStore(ICardStore&&) = delete;
  ICardStore& operator=(ICardStore&&) = delete;

  [[nodiscard]] virtual security::SecureBuffer get_blob() = 0;
  virtual void put_blob(const security::SecureBuffer& blob) = 0;
  [[nodiscard]] virtual CardCapabilities capabilities() const noexcept = 0;

 protected:
  ICardStore() noexcept = default;
};

}  // namespace usb_vault::card
