#pragma once

#include <Windows.h>

namespace usb_vault::volume_creation::detail {

class LocalSecurityDescriptor final {
 public:
  LocalSecurityDescriptor() noexcept = default;
  explicit LocalSecurityDescriptor(PSECURITY_DESCRIPTOR value) noexcept;
  ~LocalSecurityDescriptor() noexcept;

  LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
  LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;

  LocalSecurityDescriptor(LocalSecurityDescriptor&& other) noexcept;
  LocalSecurityDescriptor& operator=(LocalSecurityDescriptor&& other) noexcept;

  [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept;

 private:
  void reset() noexcept;

  PSECURITY_DESCRIPTOR value_{};
};

// UAC elevation changes the token's default owner to Administrators while the
// token user remains the launching Windows account. The non-elevated client
// needs an explicit allow ACE for that user SID; OWNER RIGHTS would grant the
// elevated object owner instead and leave the client unable to connect.
// Returns an empty descriptor on any lookup, allocation, or conversion error.
[[nodiscard]] LocalSecurityDescriptor
current_user_pipe_security_descriptor() noexcept;

}  // namespace usb_vault::volume_creation::detail
