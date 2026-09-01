#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_file_chooser.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace usb_vault::ui {

inline constexpr std::uint32_t kContainerACreationMinimumMib = 20;
inline constexpr std::uint32_t kContainerACreationMaximumMib = 1024U * 1024U;

// Complete, vendor-neutral page state. It reveals neither hardware identity,
// password role, mode, tier, B-container shape, nor volume type. Unified
// mounted state remains separate for safe cleanup; authenticated A display
// facts use the narrow status below.
enum class CardProductUiState : std::uint8_t {
  no_card = 0,
  initialization_required = 1,
  login_required = 2,
  authenticated = 3,
  unavailable = 4,
};

enum class ContainerAUiState : std::uint8_t {
  absent = 0,
  present = 1,
  mounted = 2,
};

struct ContainerAUiStatus final {
  ContainerAUiState state{ContainerAUiState::absent};
  std::uint32_t size_mib{};
  wchar_t drive_letter{};
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
  void create_container_a(std::uint32_t size_mib);

  // Creates one standard container of the requested size at a location the
  // user picks in the native chooser. Returns false when the user cancels;
  // an out-of-range size or a wrong route throws.
  bool create_container(std::uint32_t size_mib);

  // Adopts and mounts an existing container the user picks in the native
  // chooser. Returns false when the user cancels.
  bool mount_container();

  // Mounts a container this session already adopted, without asking for the
  // file again. Falls back to the chooser when this controller never saw the
  // path, which happens only for a container adopted during login.
  bool mount_container(std::uint32_t id);

  void unmount_container(std::uint32_t id);
  void unmount();

  [[nodiscard]] CardProductUiState state() const noexcept;
  [[nodiscard]] bool mounted() const noexcept;
  [[nodiscard]] ContainerAUiStatus container_a_status();

  // Live facts for every adopted container, in adoption order. Returns an
  // empty list on every route other than `authenticated`.
  [[nodiscard]] std::vector<ContainerUiStatus> container_statuses();

 private:
  application::ICardEntryActions& entry_;
  application::CardAuthenticationSession& authentication_;
  application::IAuthenticatedCardVolumeSession& volume_session_;
  const security::IPasswordStrengthPolicy& password_policy_;
  IContainerFileChooser& file_chooser_;
  // Paths this controller obtained from the chooser, kept only so an adopted
  // container can be mounted again without reselecting its file. It mirrors
  // what the session already holds, lives only in this process, and is never
  // written anywhere.
  std::map<std::uint32_t, std::wstring> chosen_paths_;
};

}  // namespace usb_vault::ui
