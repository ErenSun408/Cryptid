#pragma once

#include "volume_creation/creator_protocol.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace usb_vault::volume_creation::detail {

// Returns what the request measured, or zero for one that creates.
[[nodiscard]] std::uint64_t execute_request(HANDLE pipe, Request& request);

// Answers measurements on one pipe until the caller closes it, a request that
// is not a measurement arrives, or the session sits idle too long.
void serve_measurement_session(HANDLE pipe,
                               const std::array<std::byte, kNonceSize>& nonce,
                               Request& first);

// Both sides bind every maintenance request to the original live mount.
[[nodiscard]] inline bool matches_maintenance_binding(const Request& request,
    std::wstring_view path, std::uint32_t drive, std::int32_t id, std::uint64_t bytes) {
  return (request.operation == Operation::import_protected_file ||
          request.operation == Operation::scan_maintenance_space) &&
      request.target_path == path && request.mounted_drive == drive &&
      request.mount_id == id && request.outer_size_bytes == bytes;
}

// Explicit measurement-and-import consent, bound to the first mount identity.
void serve_maintenance_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first);

// Dedicated mount-bound import session; exits on the first failure.
void serve_import_session(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, Request& first);

}  // namespace usb_vault::volume_creation::detail
