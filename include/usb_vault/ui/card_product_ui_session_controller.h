#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_file_chooser.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace usb_vault::ui {

inline constexpr std::uint32_t kContainerCreationMinimumMib = 20;
inline constexpr std::uint32_t kContainerCreationMaximumMib = 1024U * 1024U;

// Complete, vendor-neutral page state. It reveals neither hardware identity,
// password role, mode, tier, B-container shape, nor volume type. Unified
// mounted state remains separate for safe cleanup; per-container display facts
// use the narrow status below.
enum class CardProductUiState : std::uint8_t {
  no_card = 0,
  initialization_required = 1,
  login_required = 2,
  authenticated = 3,
  unavailable = 4,
};

// One standard container adopted by the current session, projected for the
// page. The identifier is session scoped, the name is the file name alone,
// and no path, salt, password, tier, or volume type is included.
struct ContainerUiStatus final {
  std::uint32_t id{};
  std::wstring name;
  bool mounted{};
  std::uint32_t size_mib{};
  wchar_t drive_letter{};
};

// Native product UI boundary for P2. Mutable bridge inputs are wiped on every
// path. Initialization performs two independent NFC ingresses before creating
// a confirmation proof; login always accepts exactly one input and delegates
// retry/invalidation semantics to the insertion owner.
class CardProductUiSessionController final {
 public:
  CardProductUiSessionController(
      application::ICardEntryActions& entry,
      application::CardAuthenticationSession& authentication,
      application::IAuthenticatedCardVolumeSession& volume_session,
      const security::IPasswordStrengthPolicy& password_policy,
      IContainerFileChooser& file_chooser) noexcept;

  void submit_login(std::span<std::byte> mutable_utf8);
  void submit_initialization(
      std::span<std::byte> mutable_password_utf8,
      std::span<std::byte> mutable_confirmation_utf8);
  // Creates one standard container of the requested size at a location the
  // user picks in the native chooser. Returns false when the user cancels;
  // an out-of-range size or a wrong route throws.
  bool create_container(std::uint32_t size_mib);

  // Creates the single hidden-volume container and commits the second card
  // password that opens its outer volume. The caller supplies the password it
  // is currently logged in under plus the new password twice; all three
  // buffers are wiped on every path. The card record is updated first so a
  // failed volume creation leaves a retryable state instead of a container
  // nobody can open.
  void create_container_b(
      std::uint32_t outer_size_mib, std::uint32_t hidden_size_mib,
      std::span<std::byte> mutable_login_password_utf8,
      std::span<std::byte> mutable_second_password_utf8,
      std::span<std::byte> mutable_second_confirmation_utf8);

  // Adopts and mounts an existing container the user picks in the native
  // chooser. Returns false when the user cancels.
  bool mount_container();

  // Mounts a container this session already adopted. Nothing is asked of the
  // user: the session still holds everything each container needs to open.
  void mount_container(std::uint32_t id);

  void unmount_container(std::uint32_t id);
  void unmount();

  [[nodiscard]] CardProductUiState state() const noexcept;
  [[nodiscard]] bool mounted() const noexcept;
  // Live facts for every adopted container, in adoption order. Returns an
  // empty list on every route other than `authenticated`.
  [[nodiscard]] std::vector<ContainerUiStatus> container_statuses();

 private:
  application::ICardEntryActions& entry_;
  application::CardAuthenticationSession& authentication_;
  application::IAuthenticatedCardVolumeSession& volume_session_;
  const security::IPasswordStrengthPolicy& password_policy_;
  IContainerFileChooser& file_chooser_;
};

}  // namespace usb_vault::ui
