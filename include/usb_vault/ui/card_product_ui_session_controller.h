#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/confirmed_password.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace usb_vault::ui {

inline constexpr std::uint32_t kContainerACreationMinimumMib = 20;
inline constexpr std::uint32_t kContainerACreationMaximumMib = 1024U * 1024U;

// Complete, vendor-neutral page state. It reveals neither hardware identity,
// password role, mode, tier, container shape, nor volume type. Mounted state
// remains a separate unified boolean solely for safe cleanup controls.
enum class CardProductUiState : std::uint8_t {
  no_card = 0,
  initialization_required = 1,
  login_required = 2,
  authenticated = 3,
  unavailable = 4,
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
      const security::IPasswordStrengthPolicy& password_policy) noexcept;

  void submit_login(std::span<std::byte> mutable_utf8);
  void submit_initialization(
      std::span<std::byte> mutable_password_utf8,
      std::span<std::byte> mutable_confirmation_utf8);
  void create_container_a(std::uint32_t size_mib);
  void unmount();

  [[nodiscard]] CardProductUiState state() const noexcept;
  [[nodiscard]] bool mounted() const noexcept;

 private:
  application::ICardEntryActions& entry_;
  application::CardAuthenticationSession& authentication_;
  application::IAuthenticatedCardVolumeSession& volume_session_;
  const security::IPasswordStrengthPolicy& password_policy_;
};

}  // namespace usb_vault::ui
