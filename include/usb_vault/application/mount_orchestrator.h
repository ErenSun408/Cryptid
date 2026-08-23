#pragma once

#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace usb_vault::application {

struct ContainerMount final {
  std::wstring path;
  security::KdfV1Salt salt{};
  wchar_t drive_letter{};
};

struct ContainerLayout final {
  ContainerMount container_a;
  ContainerMount container_b;
};

struct MountSessionResult final {
  std::optional<volume::MountResult> container_a;
  std::optional<volume::MountResult> container_b;
  std::chrono::milliseconds elapsed{};
};

// Reports that a mount transaction failed and the driver then rejected both
// normal and forced compensation. The paths and passwords are deliberately not
// retained; callers receive only the residual drive letter and exception state.
class MountRollbackError final : public std::runtime_error {
 public:
  MountRollbackError(wchar_t residual_drive_letter,
                     std::exception_ptr operation_failure,
                     std::exception_ptr cleanup_failure);

  [[nodiscard]] wchar_t residual_drive_letter() const noexcept;
  [[nodiscard]] const std::exception_ptr& operation_failure() const noexcept;
  [[nodiscard]] const std::exception_ptr& cleanup_failure() const noexcept;

 private:
  wchar_t residual_drive_letter_{};
  std::exception_ptr operation_failure_;
  std::exception_ptr cleanup_failure_;
};

// Mounts the containers that currently exist, following the fixed B-before-A
// authentication gate. The function never persists paths, salts, passwords,
// container shape, or mount results.
[[nodiscard]] MountSessionResult mount_session(
    const volume::VolumeProvider& provider,
    const ContainerLayout& layout,
    const security::SecureBuffer& password_utf8,
    const security::SecureBuffer& card_secret);

}  // namespace usb_vault::application
