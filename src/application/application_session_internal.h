#pragma once

#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_provider.h"

namespace usb_vault::application::detail {

struct ApplicationSessionState final {
  bool container_a_mounted{false};
  bool container_b_mounted{false};

  [[nodiscard]] bool mounted() const noexcept {
    return container_a_mounted || container_b_mounted;
  }
};

class ApplicationSessionRuntime {
 public:
  virtual ~ApplicationSessionRuntime() noexcept = default;

  [[nodiscard]] virtual security::SecureBuffer get_card_blob() = 0;
  [[nodiscard]] virtual MountSessionResult mount(
      const ContainerLayout& layout,
      const security::SecureBuffer& password_utf8,
      const security::SecureBuffer& card_secret) = 0;
  [[nodiscard]] virtual volume::UnmountResult unmount(
      wchar_t drive_letter) = 0;
  // Emergency cleanup only. Escalates to a forced unmount when the normal
  // one is rejected, and reports whether force was actually required.
  [[nodiscard]] virtual volume::UnmountEscalationResult unmount_forced(
      wchar_t drive_letter) = 0;
};

void authenticate_and_mount_with_runtime(
    ApplicationSessionRuntime& runtime,
    const ContainerLayout& layout,
    ApplicationSessionState& state,
    security::SecureBuffer password_utf8);

void unmount_active_with_runtime(ApplicationSessionRuntime& runtime,
                                 const ContainerLayout& layout,
                                 ApplicationSessionState& state);

// Returns true when at least one drive needed a forced unmount.
[[nodiscard]] bool force_unmount_active_with_runtime(
    ApplicationSessionRuntime& runtime, const ContainerLayout& layout,
    ApplicationSessionState& state);

}  // namespace usb_vault::application::detail
