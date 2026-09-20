#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_target_path.h"
#include "usb_vault/ui/password_ingress.h"

#include <algorithm>
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
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

// A name that could have come off a mounted volume. Windows refuses every one
// of these characters in a file name, so a listing carrying one did not come
// from a file system this product can mount. Control characters matter twice
// over: the bridge packs a whole listing into one string and separates it with
// two of them, and a name able to hold one could make the page read a listing
// other than the one it was given.
[[nodiscard]] bool is_displayable_name(const std::wstring_view name) noexcept {
  constexpr std::wstring_view kForbidden = L"\\/:*?\"<>|";
  if (name.empty()) return false;
  for (const wchar_t character : name) {
    if (character < 0x20 || character == 0x7f) return false;
    if (kForbidden.find(character) != std::wstring_view::npos) return false;
  }
  return true;
}

// Converts a live volume capacity the page can display. A volume is never an
// exact MiB multiple - the header group costs bytes, and a hidden volume sits
// wherever the host filesystem left room - so the nearest MiB is the honest
// reading here, where refusing every inexact value would empty the field.
// Anything past the creation range is still reported as unavailable.
[[nodiscard]] std::uint32_t displayable_size_mib(
    const std::uint64_t size_bytes) noexcept {
  const std::uint64_t rounded =
      (size_bytes + kBytesPerMib / 2ULL) / kBytesPerMib;
  return rounded <= kContainerCreationMaximumMib
             ? static_cast<std::uint32_t>(rounded)
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

// The outer volume has to hold enough handover content to be credible, so
// the inner volume is capped at three quarters of the total. The page shows
// the same rule; this is the boundary that enforces it.
[[nodiscard]] bool hidden_fits_outer(
    const std::uint32_t outer_size_mib,
    const std::uint32_t hidden_size_mib) noexcept {
  return static_cast<std::uint64_t>(hidden_size_mib) * 4ULL <=
         static_cast<std::uint64_t>(outer_size_mib) * 3ULL;
}

// The one place a page-supplied string becomes a file system path. The syntax
// rules live in `resolve_container_target_path`; the one fact that cannot be
// decided from the string alone is added here, because a save location that
// does not exist has to be reported as a rejected field and not as the opaque
// creation failure the volume layer would raise several steps later.
//
// The directory is only probed, never created. Creating it would leave exactly
// the kind of local artifact section 8.4 forbids, on a path the user may have
// mistyped.
[[nodiscard]] std::wstring resolve_target_path(
    const std::wstring_view directory, const std::wstring_view name) {
  std::wstring path = resolve_container_target_path(directory, name);
  std::error_code probe;
  if (!std::filesystem::is_directory(std::filesystem::path(directory),
                                     probe)) {
    throw std::invalid_argument("save location is not an existing directory");
  }
  return path;
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

DecoyMaintenanceUiSurvey CardProductUiSessionController::survey_decoy_maintenance(
    const std::span<std::byte> mutable_password_utf8,
    const application::SurveyProgress& progress,
    const application::SurveyCancellation& cancelled) {
  MutableInputWiper input_wiper(mutable_password_utf8);
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "decoy maintenance is not available in the current UI state");
  }
  security::SecureBuffer password =
      consume_password_utf8(mutable_password_utf8);
  try {
    const application::DecoyMaintenanceSurvey survey =
        volume_session_.survey_decoy_maintenance(password, progress, cancelled);
    password.clear();
    return {.maintainable = survey.maintainable,
            .unreachable = survey.unreachable,
            .cancelled = survey.cancelled};
  } catch (...) {
    password.clear();
    throw;
  }
}

wchar_t CardProductUiSessionController::open_decoy_maintenance(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "decoy maintenance is not available in the current UI state");
  }
  return volume_session_.open_surveyed_maintenance(id);
}

DecoyMaintenanceUiSpace CardProductUiSessionController::decoy_maintenance_space(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "decoy maintenance is not available in the current UI state");
  }
  const application::DecoyMaintenanceSpace space =
      volume_session_.decoy_maintenance_space(id);
  // Rounded down, as capacity is everywhere on this page: a number that
  // rounded up would invite the write that gets refused.
  constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
  return {.total_mib = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(space.total_bytes / kMib, UINT32_MAX)),
          .free_mib = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(space.free_bytes / kMib, UINT32_MAX)),
          .writable_mib = static_cast<std::uint32_t>(std::min<std::uint64_t>(
              space.writable_bytes / kMib, UINT32_MAX))};
}

void CardProductUiSessionController::end_decoy_maintenance(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "decoy maintenance is not available in the current UI state");
  }
  volume_session_.end_decoy_maintenance(id);
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

std::wstring CardProductUiSessionController::browse_for_container_directory() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container creation is not available in the current UI state");
  }
  return file_chooser_.choose_container_directory();
}

void CardProductUiSessionController::create_container(
    const std::wstring_view name, const std::wstring_view directory,
    const std::uint32_t size_mib) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container creation is not available in the current UI state");
  }
  if (size_mib < kContainerCreationMinimumMib ||
      size_mib > kContainerCreationMaximumMib) {
    throw std::invalid_argument("container size is outside the UI range");
  }

  const std::wstring path = resolve_target_path(directory, name);
  static_cast<void>(volume_session_.create_container(
      path, static_cast<std::uint64_t>(size_mib) * kBytesPerMib));
}

void CardProductUiSessionController::create_container_b(
    const std::wstring_view name, const std::wstring_view directory,
    const std::uint32_t outer_size_mib, const std::uint32_t hidden_size_mib,
    const std::span<std::byte> mutable_second_password_utf8,
    const std::span<std::byte> mutable_second_confirmation_utf8) {
  MutableInputWiper second_wiper(mutable_second_password_utf8,
                                 mutable_second_confirmation_utf8);
  if (inputs_overlap(mutable_second_password_utf8,
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
      hidden_size_mib == 0 || hidden_size_mib >= outer_size_mib ||
      !hidden_fits_outer(outer_size_mib, hidden_size_mib)) {
    throw std::invalid_argument("container B sizes are outside the UI range");
  }

  // The location is settled before anything is derived or committed, so a
  // rejected name leaves the card and the second password untouched.
  const std::wstring path = resolve_target_path(directory, name);

  // The page no longer asks for the password the session was opened under:
  // the session still holds it, and which of the two it is stays unknown here
  // exactly as it is there.
  security::SecureBuffer login = volume_session_.copy_session_password();
  SecureBufferGuard login_guard(login);
  if (login.empty()) {
    throw_wrong_route(
        "container creation requires the session that captured the login");
  }
  security::SecureBuffer second =
      std::move(security::confirm_password(
                    password_policy_,
                    consume_password_utf8(mutable_second_password_utf8),
                    consume_password_utf8(mutable_second_confirmation_utf8)))
          .take();
  SecureBufferGuard second_guard(second);

  // The card record is committed next. A volume that exists but cannot be
  // opened with the second password would block every retry, while a second
  // password that opens nothing yet is harmless and can be set again.
  entry_.set_second_card_password(duplicate(login), duplicate(second));
  static_cast<void>(volume_session_.create_container_b(
      path, static_cast<std::uint64_t>(outer_size_mib) * kBytesPerMib,
      static_cast<std::uint64_t>(hidden_size_mib) * kBytesPerMib, second));
}

bool CardProductUiSessionController::mount_container() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting a container is not available in the current UI state");
  }

  const std::wstring path = file_chooser_.choose_existing_container();
  if (path.empty()) return false;
  // Picking a file says where a container is, not that it should be opened.
  // Unless the user asked for the mount, the import stops at the list, which
  // is also what a remembered path does at login.
  if (volume_session_.mounting_imported_containers()) {
    static_cast<void>(volume_session_.mount_container(path));
  } else {
    static_cast<void>(volume_session_.adopt_container(path));
  }
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

void CardProductUiSessionController::mount_all() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting every container is not available in the current UI state");
  }
  volume_session_.mount_adopted_all();
}

void CardProductUiSessionController::unmount_container(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "unmounting a container is not available in the current UI state");
  }
  volume_session_.unmount_container(id);
}

void CardProductUiSessionController::forget_container(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "dropping a container is not available in the current UI state");
  }
  volume_session_.forget_container(id);
}

DirectoryUiListing CardProductUiSessionController::list_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "browsing a container is not available in the current UI state");
  }
  const volume::VolumeDirectoryListing listing =
      volume_session_.list_container_directory(id, relative_directory);

  DirectoryUiListing result;
  result.truncated = listing.truncated;
  result.entries.reserve(listing.entries.size());
  for (const volume::VolumeDirectoryEntry& entry : listing.entries) {
    // 🔴 A name no Windows file system can produce is evidence of a tampered
    // volume, not a file to draw, and every projection past this point carries
    // names as text. Refusing the listing says so; drawing it would leave the
    // page describing something other than what is there.
    if (!is_displayable_name(entry.name) ||
        (entry.is_directory && entry.size_bytes != 0)) {
      throw std::runtime_error("directory entry is inconsistent");
    }
    result.entries.push_back({.name = entry.name,
                              .is_directory = entry.is_directory,
                              .is_link = entry.is_reparse_point,
                              .size_bytes = entry.size_bytes,
                              .modified_unix_ms = entry.modified_unix_ms});
  }
  return result;
}

void CardProductUiSessionController::delete_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "deleting an entry is not available in the current UI state");
  }
  volume_session_.delete_container_entry(id, relative_directory, name);
}

void CardProductUiSessionController::rename_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name, const std::wstring_view new_name) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "renaming an entry is not available in the current UI state");
  }
  volume_session_.rename_container_entry(id, relative_directory, name,
                                         new_name);
}

void CardProductUiSessionController::create_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "creating a directory is not available in the current UI state");
  }
  volume_session_.create_container_directory(id, relative_directory, name);
}

void CardProductUiSessionController::export_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "exporting an entry is not available in the current UI state");
  }
  volume_session_.export_container_entry(id, relative_directory, name,
                                         absolute_destination_directory);
}

void CardProductUiSessionController::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "migrating a container is not available in the current UI state");
  }
  volume_session_.export_container_file(id, absolute_destination_directory);
}

void CardProductUiSessionController::move_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "moving an entry is not available in the current UI state");
  }
  volume_session_.move_container_entry(source_id, source_relative_directory,
                                       name, destination_id,
                                       destination_relative_directory);
}

void CardProductUiSessionController::copy_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "copying an entry is not available in the current UI state");
  }
  volume_session_.copy_container_entry(source_id, source_relative_directory,
                                       name, destination_id,
                                       destination_relative_directory);
}

bool CardProductUiSessionController::remembering_container_paths() {
  return volume_session_.remembering_container_paths();
}

void CardProductUiSessionController::set_remembering_container_paths(
    const bool remember) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "changing this setting is not available in the current UI state");
  }
  volume_session_.set_remembering_container_paths(remember);
}

bool CardProductUiSessionController::mounting_imported_containers() {
  return volume_session_.mounting_imported_containers();
}

void CardProductUiSessionController::set_mounting_imported_containers(
    const bool mount) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "changing this setting is not available in the current UI state");
  }
  volume_session_.set_mounting_imported_containers(mount);
}

void CardProductUiSessionController::unmount() {
  volume_session_.unmount_active();
}

void CardProductUiSessionController::force_unmount_container(const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("manual container cleanup requires an authenticated session");
  volume_session_.force_unmount_container(id);
}

void CardProductUiSessionController::force_unmount_user_active() {
  volume_session_.force_unmount_user_active();
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

std::vector<application::ContainerOperationTarget> CardProductUiSessionController::batch_targets() {
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("batch selection requires authentication");
  auto targets = volume_session_.container_operation_targets();
  for (const auto& target : targets) {
    if (target.id == 0 || target.name.empty())
      throw std::runtime_error("invalid operation target");
  }
  return targets;
}

std::vector<ContainerUiStatus>
CardProductUiSessionController::container_statuses() {
  if (state() != CardProductUiState::authenticated) return {};

  const std::vector<application::ContainerStatus> statuses =
      volume_session_.container_statuses();
  std::vector<ContainerUiStatus> result;
  result.reserve(statuses.size());
  for (const application::ContainerStatus& status : statuses) {
    // Capacity and drive letter now share one rule: both describe an attached
    // volume, so both are present exactly when the container is mounted.
    if (status.id == 0 || status.name.empty() ||
        (status.mounted &&
         (status.drive_letter < L'A' || status.drive_letter > L'Z' ||
          status.size_bytes == 0)) ||
        (!status.mounted &&
         (status.drive_letter != wchar_t{} || status.size_bytes != 0))) {
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

std::vector<OperationUiEvent>
CardProductUiSessionController::operation_history() {
  if (state() != CardProductUiState::authenticated) return {};

  const std::vector<application::OperationEvent> events =
      volume_session_.operation_history();
  if (events.size() > application::kOperationHistoryCapacity) {
    throw std::runtime_error("operation history exceeds its fixed capacity");
  }
  std::vector<OperationUiEvent> result;
  result.reserve(events.size());
  for (const application::OperationEvent& event : events) {
    if (event.sequence == 0 || event.completed_at_unix_ms <= 0 ||
        event.container_name.find_first_of(L"\\/") != std::wstring::npos) {
      throw std::runtime_error("operation history event is inconsistent");
    }

    OperationUiKind kind{};
    switch (event.kind) {
      case application::OperationKind::session_opened:
        kind = OperationUiKind::session_opened;
        break;
      case application::OperationKind::container_created:
        kind = OperationUiKind::container_created;
        break;
      case application::OperationKind::container_mounted:
        kind = OperationUiKind::container_mounted;
        break;
      case application::OperationKind::container_unmounted:
        kind = OperationUiKind::container_unmounted;
        break;
      default:
        throw std::runtime_error("operation history kind is unknown");
    }

    OperationUiOutcome outcome{};
    switch (event.outcome) {
      case application::OperationOutcome::succeeded:
        outcome = OperationUiOutcome::succeeded;
        break;
      case application::OperationOutcome::failed:
        outcome = OperationUiOutcome::failed;
        break;
      default:
        throw std::runtime_error("operation history outcome is unknown");
    }

    const bool has_name = !event.container_name.empty();
    const bool has_drive =
        event.drive_letter >= L'A' && event.drive_letter <= L'Z';
    const bool no_drive = event.drive_letter == wchar_t{};
    const bool valid_shape =
        (kind == OperationUiKind::session_opened && !has_name && no_drive &&
         outcome == OperationUiOutcome::succeeded) ||
        (kind == OperationUiKind::container_created && has_name && no_drive) ||
        (kind == OperationUiKind::container_mounted && has_name &&
         ((outcome == OperationUiOutcome::succeeded && has_drive) ||
          (outcome == OperationUiOutcome::failed && no_drive))) ||
        (kind == OperationUiKind::container_unmounted && has_name && has_drive);
    if (!valid_shape) {
      throw std::runtime_error("operation history fields are inconsistent");
    }

    result.push_back({.sequence = event.sequence,
                      .completed_at_unix_ms = event.completed_at_unix_ms,
                      .kind = kind,
                      .outcome = outcome,
                      .container_name = event.container_name,
                      .drive_letter = event.drive_letter});
  }
  return result;
}

bool CardProductUiSessionController::can_reset_card_record() const noexcept {
  return state() == CardProductUiState::authenticated;
}

void CardProductUiSessionController::reset_card_record() {
  if (!can_reset_card_record()) {
    throw_wrong_route(
        "resetting the security card is not available in the current UI "
        "state");
  }
  // A mount that outlives its key material would hold a drive letter nothing
  // can release, so an attached volume stops the operation before the card is
  // written at all.
  volume_session_.unmount_active();
  if (volume_session_.mounted()) {
    throw std::system_error(
        card::make_error_code(card::CardError::invalid_operation),
        "the card cannot be reset while a volume is still mounted");
  }

  entry_.reset_card_record(authentication_);

  // Nothing derived from the destroyed identity may outlive it.
  volume_session_.release_session_secrets();
  volume_session_.clear_operation_history();
  authentication_.clear_authentication();
}

void CardProductUiSessionController::clear_operation_history() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "clearing operation history is not available in the current UI state");
  }
  volume_session_.clear_operation_history();
}

void CardProductUiSessionController::set_creation_feedback(application::CreationFeedback feedback) {
  volume_session_.set_creation_feedback(std::move(feedback));
}

void CardProductUiSessionController::set_creation_cancellation(
    application::CreationCancellation cancelled) {
  volume_session_.set_creation_cancellation(std::move(cancelled));
}

bool CardProductUiSessionController::operation_logging_enabled() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("operation logging preference requires authentication");
  }
  return volume_session_.operation_logging_enabled();
}
bool CardProductUiSessionController::force_unmount_enabled() {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  return volume_session_.force_unmount_enabled();
}
bool CardProductUiSessionController::auto_mount_enabled() {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  return volume_session_.auto_mount_enabled();
}
void CardProductUiSessionController::set_auto_mount_enabled(bool enabled) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  volume_session_.set_auto_mount_enabled(enabled);
}
void CardProductUiSessionController::set_force_unmount_enabled(bool enabled) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  volume_session_.set_force_unmount_enabled(enabled);
}

void CardProductUiSessionController::reorder_containers(std::span<const std::uint32_t> ids) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("ordering requires authentication");
  volume_session_.reorder_containers(ids);
}

void CardProductUiSessionController::set_operation_logging_enabled(bool enabled) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("operation logging preference requires authentication");
  }
  volume_session_.set_operation_logging_enabled(enabled);
}

}  // namespace usb_vault::ui
