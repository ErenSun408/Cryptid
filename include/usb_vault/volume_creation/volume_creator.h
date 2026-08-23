#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>
#include <system_error>

namespace usb_vault::volume_creation {

enum class CreationError : std::int32_t {
  invalid_request = 1,
  protocol_error = 2,
  elevation_cancelled = 3,
  helper_launch_failed = 4,
  helper_connection_failed = 5,
  helper_not_elevated = 6,
  format_backend_unavailable = 7,
  format_initialization_failed = 8,
  format_rejected = 9,
  target_exists = 10,
  target_access_denied = 11,
  mount_failed = 12,
  bitmap_scan_failed = 13,
  insufficient_tail_space = 14,
  cleanup_failed = 15,
  operation_cancelled = 16,
  io_error = 17,
  helper_terminated = 18,
};

[[nodiscard]] const std::error_category& creation_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(CreationError error) noexcept;

enum class CreationStage : std::uint32_t {
  formatting = 1,
  formatting_outer = 2,
  scanning_tail_space = 3,
  formatting_hidden = 4,
};

struct CreationProgress final {
  CreationStage stage{CreationStage::formatting};
  std::uint32_t percent{};
};

using ProgressCallback = std::function<void(const CreationProgress&)>;

// Launches the separately distributed volume-creation helper through the UAC
// consent path. Passwords are sent only through a local, per-invocation named
// pipe and are never placed in command-line arguments or environment variables.
class VolumeCreator final {
 public:
  explicit VolumeCreator(std::filesystem::path helper_path);

  void create_standard(std::wstring_view target_path,
                       std::uint64_t size_bytes,
                       const security::SecureBuffer& volume_password,
                       const ProgressCallback& progress = {}) const;

  // Creates one self-contained outer+hidden container. requested_hidden_bytes
  // may be zero to use the largest safe tail-aligned size determined from the
  // real filesystem allocation bitmap.
  void create_with_hidden(
      std::wstring_view target_path,
      std::uint64_t outer_size_bytes,
      std::uint64_t requested_hidden_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password,
      const ProgressCallback& progress = {}) const;

 private:
  std::filesystem::path helper_path_;
};

}  // namespace usb_vault::volume_creation

template <>
struct std::is_error_code_enum<usb_vault::volume_creation::CreationError>
    : true_type {};
