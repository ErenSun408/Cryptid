#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/ui/password_ingress.h"

#include <Windows.h>

#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace usb_vault::ui {
namespace {

class MutableInputWiper final {
 public:
  MutableInputWiper(const std::span<std::byte> first,
                    const std::span<std::byte> second = {}) noexcept
      : first_(first), second_(second) {}

  ~MutableInputWiper() noexcept {
    wipe(first_);
    wipe(second_);
  }

  MutableInputWiper(const MutableInputWiper&) = delete;
  MutableInputWiper& operator=(const MutableInputWiper&) = delete;

 private:
  static void wipe(const std::span<std::byte> input) noexcept {
    if (!input.empty()) {
      static_cast<void>(::SecureZeroMemory(input.data(), input.size()));
    }
  }

  std::span<std::byte> first_;
  std::span<std::byte> second_;
};

[[noreturn]] void throw_wrong_route(const char* const operation) {
  throw std::system_error(
      card::make_error_code(card::CardError::invalid_operation), operation);
}

[[nodiscard]] bool inputs_overlap(
    const std::span<const std::byte> first,
    const std::span<const std::byte> second) noexcept {
  if (first.empty() || second.empty()) return false;
  const std::uintptr_t first_address =
      reinterpret_cast<std::uintptr_t>(first.data());
  const std::uintptr_t second_address =
      reinterpret_cast<std::uintptr_t>(second.data());
  if (first_address <= second_address) {
    return second_address - first_address < first.size();
  }
  return first_address - second_address < second.size();
}

}  // namespace

CardProductUiSessionController::CardProductUiSessionController(
    application::ICardEntryActions& entry,
    application::CardAuthenticationSession& authentication,
    application::IAuthenticatedCardVolumeSession& volume_session,
    const security::IPasswordStrengthPolicy& password_policy) noexcept
    : entry_(entry),
      authentication_(authentication),
      volume_session_(volume_session),
      password_policy_(password_policy) {}

void CardProductUiSessionController::submit_login(
    const std::span<std::byte> mutable_utf8) {
  MutableInputWiper input_wiper(mutable_utf8);
  if (state() != CardProductUiState::login_required) {
    throw_wrong_route("card login is not available in the current UI state");
  }
  entry_.authenticate_login(
      volume_session_, consume_password_utf8(mutable_utf8));
}

void CardProductUiSessionController::submit_initialization(
    const std::span<std::byte> mutable_password_utf8,
    const std::span<std::byte> mutable_confirmation_utf8) {
  MutableInputWiper input_wiper(mutable_password_utf8,
                                mutable_confirmation_utf8);
  if (inputs_overlap(mutable_password_utf8,
                     mutable_confirmation_utf8)) {
    throw std::invalid_argument(
        "password and confirmation inputs must be independent");
  }
  if (state() != CardProductUiState::initialization_required) {
    throw_wrong_route(
        "card initialization is not available in the current UI state");
  }

  security::SecureBuffer password =
      consume_password_utf8(mutable_password_utf8);
  security::SecureBuffer confirmation =
      consume_password_utf8(mutable_confirmation_utf8);
  security::ConfirmedPassword confirmed = security::confirm_password(
      password_policy_, std::move(password), std::move(confirmation));
  entry_.initialize_and_authenticate(authentication_, std::move(confirmed));
}

void CardProductUiSessionController::create_container_a(
    const std::uint32_t size_mib) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container A creation is not available in the current UI state");
  }
  if (size_mib < kContainerACreationMinimumMib ||
      size_mib > kContainerACreationMaximumMib) {
    throw std::invalid_argument("container A size is outside the UI range");
  }
  constexpr std::uint64_t kBytesPerMib = 1024ULL * 1024ULL;
  volume_session_.create_container_a(
      static_cast<std::uint64_t>(size_mib) * kBytesPerMib);
}

void CardProductUiSessionController::unmount() {
  volume_session_.unmount_active();
}

CardProductUiState CardProductUiSessionController::state() const noexcept {
  if (authentication_.authenticated()) {
    return CardProductUiState::authenticated;
  }
  if (volume_session_.mounted()) {
    return CardProductUiState::unavailable;
  }

  const std::optional<application::CardEntryRoute> route =
      entry_.current_route();
  if (!route.has_value()) return CardProductUiState::no_card;
  switch (*route) {
    case application::CardEntryRoute::initialization_required:
      return CardProductUiState::initialization_required;
    case application::CardEntryRoute::login_required:
      return CardProductUiState::login_required;
    case application::CardEntryRoute::unavailable:
    default:
      return CardProductUiState::unavailable;
  }
}

bool CardProductUiSessionController::mounted() const noexcept {
  return volume_session_.mounted();
}

}  // namespace usb_vault::ui
