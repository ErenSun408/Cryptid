#pragma once

#include "card/vikey/vikey_sdk_abi.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <system_error>

namespace usb_vault::card::vikey {

constexpr std::size_t kVikeyUserSnBytes = 32;
constexpr std::size_t kVikeyApiKeyBytes = 128;
constexpr std::size_t kVikeyUserPinBytes = 8;

// Owns one authenticated ViKey user session. Credentials must already reside
// in locked SecureBuffer allocations and are copied only into short-lived,
// NUL-terminated SecureBuffer arguments required by the vendor C ABI.
class VikeySession final {
 public:
  ~VikeySession() noexcept;

  VikeySession(const VikeySession&) = delete;
  VikeySession& operator=(const VikeySession&) = delete;
  VikeySession(VikeySession&&) = delete;
  VikeySession& operator=(VikeySession&&) = delete;

  [[nodiscard]] static VikeySession open(
      const sdk::Functions& functions,
      const security::SecureBuffer& user_sn,
      const security::SecureBuffer& api_key,
      const security::SecureBuffer& user_pin);

  // Explicit close reports cleanup failures. Destruction always retries any
  // still-owned cleanup without throwing.
  [[nodiscard]] std::error_code close() noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] sdk::Index index() const noexcept;

 private:
  VikeySession(sdk::Functions functions, sdk::Index index) noexcept;

  sdk::Functions functions_{};
  sdk::Index index_{};
  bool logged_in_{true};
  bool initialized_{true};
};

}  // namespace usb_vault::card::vikey
