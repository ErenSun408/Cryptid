#pragma once

#include "card/vikey/vikey_sdk_abi.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

namespace usb_vault::card::vikey {

class VikeyCardRecordStorage;

constexpr std::size_t kVikeyUserSnBytes = 32;
constexpr std::size_t kVikeyApiKeyBytes = 128;
constexpr std::size_t kVikeyUserPinBytes = 8;
constexpr std::size_t kVikeyBlobBytes = 32;
constexpr sdk::Address kVikeyBlobAddress = 0;

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
  [[nodiscard]] std::uint32_t hardware_id() const noexcept;
  [[nodiscard]] sdk::Byte user_pin_attempts_remaining() const noexcept;
  [[nodiscard]] sdk::Byte user_pin_attempts_maximum() const noexcept;
  [[nodiscard]] security::SecureBuffer read_user_blob();
  void write_user_blob(const security::SecureBuffer& blob);

 private:
  VikeySession(sdk::Functions functions, sdk::Index index,
               std::uint32_t hardware_id,
               sdk::Byte attempts_remaining,
               sdk::Byte attempts_maximum) noexcept;

  friend class VikeyCardRecordStorage;
  [[nodiscard]] std::size_t user_data_capacity();
  [[nodiscard]] security::SecureBuffer read_user_data(
      std::size_t offset, std::size_t length);
  void write_user_data(std::size_t offset,
                       std::span<const std::byte> bytes);
  void verify_hardware_identity() const;

  sdk::Functions functions_{};
  sdk::Index index_{};
  std::uint32_t hardware_id_{};
  sdk::Byte attempts_remaining_{};
  sdk::Byte attempts_maximum_{};
  bool logged_in_{true};
  bool initialized_{true};
};

}  // namespace usb_vault::card::vikey
