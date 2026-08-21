#pragma once

#include <cstddef>
#include <span>

namespace usb_vault::security {

// Owns a fixed-size, page-locked Windows allocation for short-lived secrets.
// Destruction clears the requested bytes before unlocking and releasing them.
// Locking reduces paging exposure; it does not defend against a compromised
// process, privileged memory inspection, crash dumps, or hibernation capture.
class SecureBuffer final {
 public:
  using value_type = std::byte;

  SecureBuffer() noexcept = default;
  explicit SecureBuffer(std::size_t size);

  ~SecureBuffer() noexcept;

  SecureBuffer(const SecureBuffer&) = delete;
  SecureBuffer& operator=(const SecureBuffer&) = delete;

  SecureBuffer(SecureBuffer&& other) noexcept;
  SecureBuffer& operator=(SecureBuffer&& other) noexcept;

  [[nodiscard]] value_type* data() noexcept;
  [[nodiscard]] const value_type* data() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  [[nodiscard]] std::span<value_type> bytes() noexcept;
  [[nodiscard]] std::span<const value_type> bytes() const noexcept;

  void clear() noexcept;

 private:
  void release() noexcept;

  value_type* data_{nullptr};
  std::size_t size_{0};
};

}  // namespace usb_vault::security
