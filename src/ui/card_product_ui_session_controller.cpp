#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/password_ingress.h"

#include <Windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

constexpr std::uint64_t kBytesPerMib = 1024ULL * 1024ULL;

// Converts a backing-file size the page can display. Anything that is not an
// exact MiB multiple inside the creation range is reported as unavailable
// instead of rounded, so the page never shows an invented capacity.
[[nodiscard]] std::uint32_t displayable_size_mib(
    const std::uint64_t size_bytes) noexcept {
  if (size_bytes % kBytesPerMib != 0) return 0;
  const std::uint64_t quotient = size_bytes / kBytesPerMib;
  return quotient <= kContainerCreationMaximumMib
             ? static_cast<std::uint32_t>(quotient)
             : 0;
}

// Both secrets are needed twice: once for the card record, which consumes
// them, and once for the volume creation, which only reads them. SecureBuffer
// is deliberately not copyable, so the duplicate is explicit and is cleared on
// every path together with the original.
[[nodiscard]] security::SecureBuffer duplicate(
    const security::SecureBuffer& source) {
  security::SecureBuffer result(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    result.bytes()[index] = source.bytes()[index];
  }
  return result;
}

class SecureBufferGuard final {
 public:
  explicit SecureBufferGuard(security::SecureBuffer& buffer) noexcept
      : buffer_(buffer) {}
  ~SecureBufferGuard() noexcept { buffer_.clear(); }

  SecureBufferGuard(const SecureBufferGuard&) = delete;
  SecureBufferGuard& operator=(const SecureBufferGuard&) = delete;

 private:
  security::SecureBuffer& buffer_;
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
    const security::IPasswordStrengthPolicy& password_policy,
    IContainerFileChooser& file_chooser) noexcept
    : entry_(entry),
      authentication_(authentication),
      volume_session_(volume_session),
      password_policy_(password_policy),
      file_chooser_(file_chooser) {}

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

bool CardProductUiSessionController::create_container(
    const std::uint32_t size_mib) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container creation is not available in the current UI state");
  }
  if (size_mib < kContainerCreationMinimumMib ||
      size_mib > kContainerCreationMaximumMib) {
    throw std::invalid_argument("container size is outside the UI range");
  }

  const std::wstring path = file_chooser_.choose_new_container();
  if (path.empty()) return false;
  static_cast<void>(volume_session_.create_container(
      path, static_cast<std::uint64_t>(size_mib) * kBytesPerMib));
  return true;
}

void CardProductUiSessionController::create_container_b(
    const std::uint32_t outer_size_mib, const std::uint32_t hidden_size_mib,
    const std::span<std::byte> mutable_login_password_utf8,
    const std::span<std::byte> mutable_second_password_utf8,
    const std::span<std::byte> mutable_second_confirmation_utf8) {
  MutableInputWiper login_wiper(mutable_login_password_utf8);
  MutableInputWiper second_wiper(mutable_second_password_utf8,
                                 mutable_second_confirmation_utf8);
  if (inputs_overlap(mutable_login_password_utf8,
                     mutable_second_password_utf8) ||
      inputs_overlap(mutable_login_password_utf8,
                     mutable_second_confirmation_utf8) ||
      inputs_overlap(mutable_second_password_utf8,
                     mutable_second_confirmation_utf8)) {
    throw std::invalid_argument(
        "container B inputs must be independent buffers");
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container creation is not available in the current UI state");
  }
  if (outer_size_mib < kContainerCreationMinimumMib ||
      outer_size_mib > kContainerCreationMaximumMib ||
      hidden_size_mib == 0 || hidden_size_mib >= outer_size_mib) {
    throw std::invalid_argument("container B sizes are outside the UI range");
  }

  security::SecureBuffer login =
      consume_password_utf8(mutable_login_password_utf8);
  SecureBufferGuard login_guard(login);
  security::SecureBuffer second =
      std::move(security::confirm_password(
                    password_policy_,
                    consume_password_utf8(mutable_second_password_utf8),
                    consume_password_utf8(mutable_second_confirmation_utf8)))
          .take();
  SecureBufferGuard second_guard(second);

  // The card record is committed first. A volume that exists but cannot be
  // opened with the second password would block every retry, while a second
  // password that opens nothing yet is harmless and can be set again.
  entry_.set_second_card_password(duplicate(login), duplicate(second));
  static_cast<void>(volume_session_.create_container_b(
      static_cast<std::uint64_t>(outer_size_mib) * kBytesPerMib,
      static_cast<std::uint64_t>(hidden_size_mib) * kBytesPerMib, login,
      second));
}

bool CardProductUiSessionController::mount_container() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting a container is not available in the current UI state");
  }

  const std::wstring path = file_chooser_.choose_existing_container();
  if (path.empty()) return false;
  static_cast<void>(volume_session_.mount_container(path));
  return true;
}

void CardProductUiSessionController::mount_container(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting a container is not available in the current UI state");
  }
  volume_session_.mount_adopted_container(id);
}

void CardProductUiSessionController::unmount_container(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "unmounting a container is not available in the current UI state");
  }
  volume_session_.unmount_container(id);
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

std::vector<ContainerUiStatus>
CardProductUiSessionController::container_statuses() {
  if (state() != CardProductUiState::authenticated) return {};

  const std::vector<application::ContainerStatus> statuses =
      volume_session_.container_statuses();
  std::vector<ContainerUiStatus> result;
  result.reserve(statuses.size());
  for (const application::ContainerStatus& status : statuses) {
    if (status.id == 0 || status.name.empty() ||
        (status.mounted &&
         (status.drive_letter < L'A' || status.drive_letter > L'Z')) ||
        (!status.mounted && status.drive_letter != wchar_t{})) {
      throw std::runtime_error("container status is inconsistent");
    }
    result.push_back({.id = status.id,
                      .name = status.name,
                      .mounted = status.mounted,
                      .size_mib = displayable_size_mib(status.size_bytes),
                      .drive_letter = status.drive_letter});
  }
  return result;
}

}  // namespace usb_vault::ui
