#include "usb_vault/ui/card_product_ui_session_controller.h"
#include "card/diagnostic_error.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "usb_vault/backup/transfer_package_v1.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_target_path.h"
#include "usb_vault/ui/password_ingress.h"
#include "usb_vault/volume/volume_space.h"
#include "usb_vault/volume/volume_entry_path.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <algorithm>
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
// rules live in `resolve_protected_container_target_path`; the one fact that cannot be
// decided from the string alone is added here, because a save location that
// does not exist has to be reported as a rejected field and not as the opaque
// creation failure the volume layer would raise several steps later.
//
// The chosen directory is only probed, never created. Creating it would leave
// exactly the kind of local artifact section 8.4 forbids, on a path the user
// may have mistyped. The one folder that does get created is the container's
// own, inside it, and the session makes and removes that one.
[[nodiscard]] std::wstring resolve_target_path(
    const std::wstring_view directory, const std::wstring_view name,
    const std::uint64_t required_bytes) {
  std::wstring path = resolve_protected_container_target_path(directory, name);
  const std::wstring location(directory);
  std::error_code probe;
  if (!std::filesystem::is_directory(std::filesystem::path(location), probe)) {
    throw std::invalid_argument("save location is not an existing directory");
  }
  // The container is written at its full size, so a location that cannot hold
  // it fails somewhere inside the format -- after a partial file exists and
  // after the user has waited. Asking first turns that into a refusal that
  // names the cause. It is a check, not a reservation: the space can still go
  // away before the write, which the volume layer continues to report.
  if (volume::writable_free_bytes(location) < required_bytes) {
    throw std::system_error(
        volume_creation::make_error_code(
            volume_creation::CreationError::destination_full),
        "save location has less free space than the container needs");
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
    IContainerFileChooser& file_chooser,
    ILoginResetMountAccess* reset_mount_access,
    application::IActivationGate* activation,
    application::ICardIdentityVerifier* identity_verifier) noexcept
    : reset_mount_access_(reset_mount_access), activation_(activation),
      identity_verifier_(identity_verifier), entry_(entry),
      authentication_(authentication),
      volume_session_(volume_session),
      password_policy_(password_policy),
      file_chooser_(file_chooser) {}

void CardProductUiSessionController::set_identity_lost_handler(
    std::function<void()> handler) {
  identity_lost_ = std::move(handler);
}

void CardProductUiSessionController::require_same_card() {
  // Outside an authenticated session the operation's own route check refuses.
  if (identity_verifier_ == nullptr || !authentication_.authenticated() ||
      !authenticated_card_) {
    return;
  }
  application::CardIdentityRecheckPolicy policy;
  for (;;) {
    const application::CardIdentityRecheck result =
        identity_verifier_->recheck_before_operation(*authenticated_card_);
    switch (policy.observe(result)) {
      case application::CardIdentityVerdict::present:
        return;
      case application::CardIdentityVerdict::retry:
        continue;
      case application::CardIdentityVerdict::skipped:
        USB_VAULT_TRACE("安全卡", "操作前读卡复核未完成", "结果 " + std::to_string(static_cast<int>(result)));
        throw std::system_error(
            card::make_error_code(card::CardError::communication_failed),
            "the card could not be read again before the operation");
      case application::CardIdentityVerdict::lost:
        USB_VAULT_TRACE("安全卡", "操作前读卡复核按拔卡处理", "结果 " + std::to_string(static_cast<int>(result)));
        if (identity_lost_) {
          try {
            identity_lost_();
          } catch (...) {
            USB_VAULT_TRACE_FAILURE("ui/card_product_ui_session_controller", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
          }
        }
        throw std::system_error(
            card::make_error_code(card::CardError::device_not_found),
            "the authenticated card is no longer present");
    }
  }
}

// Card errors carry a stage, a native code and a security step beyond the
// error number; the runtime log keeps all of them. Call inside a catch block.
[[nodiscard]] std::string card_failure_codes() {
  try {
    throw;
  } catch (const card::DiagnosticError& error) {
    USB_VAULT_TRACE_FAILURE("ui/card_product_ui_session_controller", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return "诊断阶段 " + std::to_string(static_cast<std::uint32_t>(error.stage())) + "；原生码 " +
           std::to_string(error.native()) + "（" + diagnostics::hex_text(error.native()) +
           "）；原生码类型 " + std::to_string(static_cast<std::uint32_t>(error.kind())) +
           "；安全步骤 " + std::to_string(error.step());
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/card_product_ui_session_controller", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return {};
  }
}

void CardProductUiSessionController::submit_login(
    const std::span<std::byte> mutable_utf8) {
  MutableInputWiper input_wiper(mutable_utf8);
  if (state() != CardProductUiState::login_required) {
    USB_VAULT_TRACE("登录", "当前状态不能登录", "状态 " + std::to_string(static_cast<int>(state())));
    throw_wrong_route("card login is not available in the current UI state");
  }
  USB_VAULT_TRACE("登录", "开始登录", "输入长度 " + std::to_string(mutable_utf8.size()) + " 字节");
  // The insertion is consumed by a successful login, so its identity is
  // taken first and kept for the session's activation display and renewal.
  // Set before the call: authentication can be established and the
  // container hand-off still throw, and that session belongs to this card.
  authenticated_card_ = entry_.current_card_identity();
  try {
    entry_.authenticate_login(
        volume_session_, consume_password_utf8(mutable_utf8));
  } catch (...) {
    if (!authentication_.authenticated()) authenticated_card_.reset();
    USB_VAULT_TRACE_FAILURE("登录", "登录失败", card_failure_codes());
    throw;
  }
  USB_VAULT_TRACE("登录", "登录成功", "状态 " + std::to_string(static_cast<int>(state())));
}

std::vector<std::uint32_t> CardProductUiSessionController::decoy_maintenance_candidates() {
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("maintenance candidates require authentication");
  return volume_session_.decoy_maintenance_candidates();
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
  require_same_card();
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
  return {.total_bytes = space.total_bytes,
          .free_bytes = space.free_bytes,
          .safe_free_bytes = space.safe_free_bytes};
}

bool CardProductUiSessionController::decoy_maintenance_write_blocked(const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("decoy maintenance is not available in the current UI state");
  return volume_session_.decoy_maintenance_write_blocked(id);
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
    USB_VAULT_TRACE("初始化", "当前状态不能初始化", "状态 " + std::to_string(static_cast<int>(state())));
    throw_wrong_route(
        "card initialization is not available in the current UI state");
  }
  USB_VAULT_TRACE("初始化", "开始初始化安全卡",
                  "口令长度 " + std::to_string(mutable_password_utf8.size()) + " 字节；确认长度 " +
                      std::to_string(mutable_confirmation_utf8.size()) + " 字节");

  security::SecureBuffer password =
      consume_password_utf8(mutable_password_utf8);
  security::SecureBuffer confirmation =
      consume_password_utf8(mutable_confirmation_utf8);
  // Keep only a locked native copy, and hand it off only after independent
  // confirmation and a verified card initialization have both succeeded.
  security::SecureBuffer session_password = duplicate(password);
  SecureBufferGuard session_password_guard(session_password);
  security::ConfirmedPassword confirmed = [&] {
    try {
      return security::confirm_password(password_policy_, std::move(password), std::move(confirmation));
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("初始化", "口令未通过确认或策略", "");
      throw;
    }
  }();
  authenticated_card_ = entry_.current_card_identity();
  try {
    entry_.initialize_and_authenticate(authentication_, std::move(confirmed));
  } catch (...) {
    if (!authentication_.authenticated()) authenticated_card_.reset();
    USB_VAULT_TRACE_FAILURE("初始化", "安全卡初始化失败", card_failure_codes());
    throw;
  }
  USB_VAULT_TRACE("初始化", "安全卡已初始化并认证", "");
  try {
    volume_session_.complete_initialization(std::move(session_password));
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("初始化", "初始化交接失败", "");
    volume_session_.release_session_secrets();
    authentication_.clear_authentication();
    throw;
  }
}

std::wstring CardProductUiSessionController::browse_for_container_directory() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "container creation is not available in the current UI state");
  }
  return file_chooser_.choose_container_directory();
}

bool CardProductUiSessionController::begin_partition_conversion(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  if (state()!=CardProductUiState::authenticated) throw std::logic_error("authentication required");
  require_same_card();
  return volume_session_.begin_partition_conversion(target,confirm);
}
bool CardProductUiSessionController::resume_partition_conversion(std::uint32_t id) {
  if (state()!=CardProductUiState::authenticated) throw std::logic_error("authentication required");
  require_same_card();
  return volume_session_.resume_partition_conversion(id);
}
void CardProductUiSessionController::set_creation_revocation(application::CreationCancellation revoked) {
  volume_session_.set_creation_revocation(std::move(revoked));
}

void CardProductUiSessionController::create_partition(
    const volume::PartitionCandidate& target, const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("partition creation requires authentication");
  require_same_card();
  static_cast<void>(volume_session_.create_partition(target, confirm));
}

std::uint32_t CardProductUiSessionController::create_container(
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
  require_same_card();

  const auto size_bytes = static_cast<std::uint64_t>(size_mib) * kBytesPerMib;
  const std::wstring path = resolve_target_path(directory, name, size_bytes);
  return volume_session_.create_protected_container(
      path, size_bytes, container_display_name(name));
}

std::uint32_t CardProductUiSessionController::create_container_b(
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
  require_same_card();

  // The location is settled before anything is derived or committed, so a
  // rejected name leaves the card and the second password untouched.
  const auto outer_size_bytes = static_cast<std::uint64_t>(outer_size_mib) * kBytesPerMib;
  const std::wstring path = resolve_target_path(directory, name, outer_size_bytes);

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
  return volume_session_.create_protected_container_b(
      path, outer_size_bytes,
      static_cast<std::uint64_t>(hidden_size_mib) * kBytesPerMib, second,
      container_display_name(name));
}

void CardProductUiSessionController::create_partition_pair(
    const volume::PartitionCandidate& target, const std::uint32_t hidden_size_mib,
    const std::span<std::byte> mutable_second_password_utf8,
    const std::span<std::byte> mutable_second_confirmation_utf8,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
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
  const auto hidden_bytes = static_cast<std::uint64_t>(hidden_size_mib) * kBytesPerMib;
  if (!confirm || !volume_session_.remembering_container_paths() || hidden_size_mib == 0 ||
      hidden_bytes > target.length_bytes / 4 * 3 ||
      target.availability != volume::PartitionAvailability::candidate ||
      !volume::matches_partition_snapshot(target, {&target, 1}))
    throw std::invalid_argument("invalid partition pair target or size");
  require_same_card();

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
  static_cast<void>(volume_session_.create_partition_pair(target, hidden_bytes, second, confirm));
}

bool CardProductUiSessionController::mount_container() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting a container is not available in the current UI state");
  }

  const std::wstring path = file_chooser_.choose_existing_container();
  if (path.empty()) return false;
  adopt_or_mount_container_file(path);
  return true;
}

void CardProductUiSessionController::adopt_or_mount_container_file(const std::wstring& path) {
  // After the pick rather than before it: the picker can stay open for long.
  require_same_card();
  std::vector<application::ContainerStatus> before = volume_session_.container_statuses();
  // Picking a file says where a container is, not that it should be opened.
  // Unless the user asked for the mount, the import stops at the list, which
  // is also what a remembered path does at login. A mounting import still
  // joins the list and takes its name first, so the mount hands the driver
  // the name the list shows rather than the file's.
  const bool mounting = volume_session_.mounting_imported_containers();
  const std::uint32_t id = volume_session_.adopt_container(path);
  const bool fresh = std::ranges::none_of(before, [id](const auto& status) { return status.id == id; });
  if (fresh) {
    // A container new to the list is shown as "本地磁盘", then "本地磁盘 2",
    // "本地磁盘 3" and so on, ignoring case, against every name already in
    // the list. Up to "本地磁盘 99" the name fits the 11 bytes a FAT label
    // holds under code page 936, so Explorer shows the same name. It is only
    // the display alias; the file keeps its own name. Failing to store it
    // leaves the file name showing and does not undo the import.
    constexpr std::wstring_view base = L"本地磁盘";
    std::wstring candidate(base);
    const auto taken = [&](const application::ContainerStatus& status) {
      return status.id != id &&
             ::CompareStringOrdinal(status.name.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    for (unsigned int ordinal = 2; std::ranges::any_of(before, taken); ++ordinal) {
      candidate = std::wstring(base) + L" " + std::to_wstring(ordinal);
    }
    try {
      volume_session_.rename_container(id, candidate);
    } catch (...) {
    }
  }
  if (!mounting) return;
  try {
    volume_session_.mount_adopted_container(id);
  } catch (...) {
    // A mounting import that fails leaves the list as it found it.
    if (fresh) {
      try {
        volume_session_.forget_container(id);
      } catch (...) {
      }
    }
    throw;
  }
}

namespace {

// Product file containers are named "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
// with no extension (resolve_protected_container_target_path).
[[nodiscard]] bool has_container_guid_name(const std::wstring& name) {
  if (name.size() != 38 || name.front() != L'{' || name.back() != L'}') return false;
  for (std::size_t index = 1; index < 37; ++index) {
    const wchar_t character = name[index];
    if (index == 9 || index == 14 || index == 19 || index == 24) {
      if (character != L'-') return false;
    } else if (!((character >= L'0' && character <= L'9') || (character >= L'A' && character <= L'F') ||
                 (character >= L'a' && character <= L'f'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool has_container_extension(const std::wstring& name) {
  const std::size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos) return false;
  std::wstring extension = name.substr(dot + 1);
  for (wchar_t& character : extension) {
    if (character >= L'A' && character <= L'Z') character = static_cast<wchar_t>(character - L'A' + L'a');
  }
  return extension == L"hc" || extension == L"sys" || extension == L"dat" || extension == L"bin";
}

}  // namespace

void CardProductUiSessionController::delete_container_from_disk(const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("deleting a container is not available in the current UI state");
  }
  volume_session_.delete_container_from_disk(id);
}

CardProductUiSessionController::ImportFileChoice
CardProductUiSessionController::choose_import_file() {
  {
    const std::scoped_lock lock(import_mutex_);
    undecided_import_file_.clear();
    pending_import_package_.clear();
    pending_import_restored_name_.clear();
    verified_import_.reset();
    verified_sender_.reset();
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("importing is not available in the current UI state");
  }
  std::wstring path = file_chooser_.choose_existing_container();
  if (path.empty()) return {};
  const std::size_t separator = path.find_last_of(L"\\/");
  std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
  if (name.empty()) return {};
  std::error_code error;
  // Only a regular file can be a container or a package; the picker offers
  // files only, and this holds even if something else reaches here.
  if (!std::filesystem::is_regular_file(path, error)) {
    if (!error) error = std::make_error_code(std::errc::not_a_directory);
    throw std::system_error(error, "the file picked for import is not a regular file");
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) throw std::system_error(error, "inspect the file picked for import");
  if (size % 512U != 0) {
    const std::scoped_lock lock(import_mutex_);
    pending_import_package_ = std::move(path);
    pending_import_generation_ = authentication_.generation();
    return {ImportFileKind::package, std::move(name)};
  }
  if (has_container_extension(name) || has_container_guid_name(name)) {
    adopt_or_mount_container_file(path);
    return {ImportFileKind::container, std::move(name)};
  }
  const std::scoped_lock lock(import_mutex_);
  undecided_import_file_ = std::move(path);
  undecided_import_generation_ = authentication_.generation();
  return {ImportFileKind::undecided, std::move(name)};
}

bool CardProductUiSessionController::adopt_undecided_import_file() {
  std::wstring path;
  {
    const std::scoped_lock lock(import_mutex_);
    path = std::move(undecided_import_file_);
    undecided_import_file_.clear();
    if (undecided_import_generation_ != authentication_.generation()) path.clear();
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("importing is not available in the current UI state");
  }
  if (path.empty()) return false;
  adopt_or_mount_container_file(path);
  return true;
}

std::wstring CardProductUiSessionController::treat_undecided_import_as_package() {
  const std::scoped_lock lock(import_mutex_);
  std::wstring path = std::move(undecided_import_file_);
  undecided_import_file_.clear();
  if (path.empty() || state() != CardProductUiState::authenticated ||
      undecided_import_generation_ != authentication_.generation()) {
    return {};
  }
  const std::size_t separator = path.find_last_of(L"\\/");
  std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
  pending_import_package_ = std::move(path);
  pending_import_generation_ = authentication_.generation();
  return name;
}

void CardProductUiSessionController::mount_container(
    const std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "mounting a container is not available in the current UI state");
  }
  require_same_card();
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
  require_same_card();
  volume_session_.export_container_entry(id, relative_directory, name,
                                         absolute_destination_directory);
}

void CardProductUiSessionController::export_container_folder(
    const std::uint32_t id, const std::wstring_view absolute_destination_directory,
    const std::wstring_view folder_name) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "migrating a container is not available in the current UI state");
  }
  require_same_card();
  volume_session_.export_container_folder(id, absolute_destination_directory, folder_name);
}

void CardProductUiSessionController::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "migrating a container is not available in the current UI state");
  }
  require_same_card();
  volume_session_.export_container_file(id, absolute_destination_directory);
}

void CardProductUiSessionController::export_container_selection(std::span<const application::ExportSelectionEntry> entries,
      std::wstring_view destination_directory, std::wstring_view folder_name, bool encrypted,
      std::span<std::byte> mutable_password_utf8, std::span<std::byte> mutable_confirmation_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
  MutableInputWiper wiper(mutable_password_utf8, mutable_confirmation_utf8);
  if (inputs_overlap(mutable_password_utf8, mutable_confirmation_utf8))
    throw std::invalid_argument("export password inputs must be independent buffers");
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("selection export requires authentication");
  require_same_card();
  security::SecureBuffer password;
  if (encrypted) {
    password = std::move(security::confirm_password(password_policy_,
        consume_password_utf8(mutable_password_utf8),
        consume_password_utf8(mutable_confirmation_utf8))).take();
  } else if (!mutable_password_utf8.empty() || !mutable_confirmation_utf8.empty()) {
    throw std::invalid_argument("plaintext export does not accept a password");
  }
  SecureBufferGuard guard(password);
  volume_session_.export_container_selection(entries, destination_directory, folder_name,
      encrypted, password, cancelled, progress);
}

void CardProductUiSessionController::export_container_entry_encrypted(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory,
    const std::wstring_view package_name,
    const std::span<std::byte> mutable_password_utf8,
    const std::span<std::byte> mutable_confirmation_utf8,
    const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
  MutableInputWiper wiper(mutable_password_utf8, mutable_confirmation_utf8);
  if (inputs_overlap(mutable_password_utf8, mutable_confirmation_utf8)) {
    throw std::invalid_argument("export password inputs must be independent buffers");
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "exporting an entry is not available in the current UI state");
  }
  require_same_card();
  security::SecureBuffer password =
      std::move(security::confirm_password(
                    password_policy_, consume_password_utf8(mutable_password_utf8),
                    consume_password_utf8(mutable_confirmation_utf8)))
          .take();
  SecureBufferGuard password_guard(password);
  volume_session_.export_container_entry_encrypted(
      id, relative_directory, name, absolute_destination_directory, package_name,
      password, cancelled, progress);
}

void CardProductUiSessionController::release_stale_import_state() noexcept {
  const std::scoped_lock lock(import_mutex_);
  const bool current = state() == CardProductUiState::authenticated;
  if (!current || pending_import_generation_ != authentication_.generation()) {
    pending_import_package_.clear();
    pending_import_restored_name_.clear();
  }
  if (!current || undecided_import_generation_ != authentication_.generation()) {
    undecided_import_file_.clear();
  }
  if (verified_import_ && (!current || verified_import_->generation != authentication_.generation())) {
    verified_import_.reset();
  }
  if (!current || verified_sender_generation_ != authentication_.generation()) {
    verified_sender_.reset();
  }
}

namespace {

class FileHandle final {
 public:
  explicit FileHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FileHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) static_cast<void>(::CloseHandle(handle_));
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[noreturn]] void throw_last_error(const char* const operation) {
  throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), operation);
}

// A key file is two short lines; anything longer is not one, and is read no
// further than that. Opened once and checked on that handle, so what is read
// is the disk file that was checked and not a pipe or device swapped in.
// A directory does not open without backup semantics.
[[nodiscard]] std::optional<std::string> read_contact_key_file(const std::wstring& path) {
  const FileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) throw_last_error("open the picked key file");
  if (::GetFileType(file.get()) != FILE_TYPE_DISK) {
    throw std::system_error(std::make_error_code(std::errc::not_a_directory),
                            "the picked key file is not a regular file");
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file.get(), &size) == FALSE) throw_last_error("inspect the picked key file");
  if (size.QuadPart > static_cast<LONGLONG>(contacts::kContactKeyFileMaxBytes)) return std::nullopt;
  std::string text(contacts::kContactKeyFileMaxBytes + 1, '\0');
  std::size_t length = 0;
  for (;;) {
    DWORD read = 0;
    if (::ReadFile(file.get(), text.data() + length, static_cast<DWORD>(text.size() - length),
                   &read, nullptr) == FALSE) {
      throw_last_error("read the picked key file");
    }
    if (read == 0) break;
    length += read;
    // Grown since it was measured: no longer a key file.
    if (length > contacts::kContactKeyFileMaxBytes) return std::nullopt;
  }
  text.resize(length);
  return text;
}

}  // namespace

void CardProductUiSessionController::set_contact_book_access(ContactBookAccess access) {
  contact_access_ = std::move(access);
}

std::shared_ptr<application::CardContactBook>
CardProductUiSessionController::open_contact_book() {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("the contact list is not available in the current UI state");
  }
  if (!contact_access_.storage || !contact_access_.lease || !authenticated_card_) {
    throw std::logic_error("the contact list is unavailable");
  }
  const std::uint64_t generation = authentication_.generation();
  {
    const std::scoped_lock lock(contact_mutex_);
    if (contact_book_ && contact_book_generation_ == generation) return contact_book_;
  }
  auto book = std::make_shared<application::CardContactBook>(
      authentication_, contact_access_.storage(*authenticated_card_), contact_access_.lease);
  std::shared_ptr<application::CardContactBook> dropped;
  const std::scoped_lock lock(contact_mutex_);
  // A login that ended while the book was opened keeps nothing of it: once
  // the session change has passed, nothing else would drop it, and its keys
  // would stay in memory until the next one.
  if (!login_still_current(generation)) {
    throw_wrong_route("the login changed while the contact list was opened");
  }
  dropped = std::move(contact_book_);
  contact_book_ = book;
  contact_book_generation_ = generation;
  return book;
}

bool CardProductUiSessionController::login_still_current(
    const std::uint64_t generation) const noexcept {
  return state() == CardProductUiState::authenticated &&
         authentication_.generation() == generation;
}

ContactBookUiView CardProductUiSessionController::contact_book_view(
    const application::CardContactBook& book) const {
  ContactBookUiView view{.state = book.state(), .own_fingerprint = book.own_fingerprint()};
  view.entries.reserve(book.entries().size());
  for (const contacts::ContactEntry& entry : book.entries()) {
    view.entries.push_back({.public_key = entry.public_key,
                            .remark = entry.remark,
                            .fingerprint = contacts::format_contact_fingerprint(entry.public_key)});
  }
  return view;
}

ContactBookUiView CardProductUiSessionController::contact_book() {
  const auto book = open_contact_book();
  if (book->state() == contacts::ContactStoreState::unread) static_cast<void>(book->load());
  return contact_book_view(*book);
}

ContactBookUiView CardProductUiSessionController::reload_contact_book() {
  const auto book = open_contact_book();
  static_cast<void>(book->load());
  return contact_book_view(*book);
}

template <typename Write>
decltype(auto) CardProductUiSessionController::write_contact_book(Write&& write) {
  // Not a card error on its own; the page names it like any unsupported card.
  const auto too_small = [] {
    return std::system_error(card::make_error_code(card::CardError::unsupported),
                             "the card user area is too small for the contact list");
  };
  const auto book = open_contact_book();
  // The recheck skips itself without a verifier; a card write does not.
  if (identity_verifier_ == nullptr) throw std::logic_error("the contact list cannot recheck the card");
  require_same_card();
  try {
    if (book->state() == contacts::ContactStoreState::unread) static_cast<void>(book->load());
    if (book->state() == contacts::ContactStoreState::unavailable) throw too_small();
    return write(*book);
  } catch (const contacts::ContactStoreCapacityError&) {
    throw too_small();
  }
}

std::optional<ContactKeyUiPreview> CardProductUiSessionController::choose_contact_key_file() {
  {
    const std::scoped_lock lock(contact_mutex_);
    pending_contact_key_.clear();
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("the contact list is not available in the current UI state");
  }
  // The dialog runs a message loop; a login that ends meanwhile gets nothing.
  const std::uint64_t generation = authentication_.generation();
  const std::wstring path = file_chooser_.choose_contact_key_file();
  if (path.empty()) return std::nullopt;
  if (!login_still_current(generation)) {
    throw_wrong_route("the login changed while the key file was picked");
  }
  const std::optional<std::string> text = read_contact_key_file(path);
  if (!text) return ContactKeyUiPreview{.error = contacts::ContactKeyFileError::malformed};
  const contacts::ContactKeyFileDecoding decoded = contacts::decode_contact_key_file(*text);
  if (!decoded.public_key) return ContactKeyUiPreview{.error = decoded.error};
  const std::scoped_lock lock(contact_mutex_);
  pending_contact_key_ = *text;
  pending_contact_key_generation_ = generation;
  return ContactKeyUiPreview{.fingerprint = contacts::format_contact_fingerprint(*decoded.public_key)};
}

application::ContactBookOutcome CardProductUiSessionController::add_chosen_contact(
    const std::string_view remark) {
  std::string text;
  {
    const std::scoped_lock lock(contact_mutex_);
    if (pending_contact_key_.empty() ||
        pending_contact_key_generation_ != authentication_.generation()) {
      pending_contact_key_.clear();
      throw std::logic_error("no key file of this login is held");
    }
    text = pending_contact_key_;
  }
  const application::ContactBookOutcome outcome =
      write_contact_book([&](application::CardContactBook& book) { return book.add(text, remark); });
  using application::ContactBookChange;
  if (outcome.change != ContactBookChange::empty_remark &&
      outcome.change != ContactBookChange::remark_too_long &&
      outcome.change != ContactBookChange::invalid_remark) {
    const std::scoped_lock lock(contact_mutex_);
    if (pending_contact_key_ == text) pending_contact_key_.clear();
  }
  return outcome;
}

application::ContactBookOutcome CardProductUiSessionController::rename_contact(
    const contacts::ContactPublicKey& key, const std::string_view remark) {
  return write_contact_book(
      [&](application::CardContactBook& book) { return book.rename(key, remark); });
}

application::ContactBookOutcome CardProductUiSessionController::remove_contact(
    const contacts::ContactPublicKey& key) {
  return write_contact_book([&](application::CardContactBook& book) { return book.remove(key); });
}

void CardProductUiSessionController::clear_contact_book() {
  write_contact_book([](application::CardContactBook& book) { book.discard_and_clear(); });
}

bool CardProductUiSessionController::export_own_contact_key() {
  const std::uint64_t generation = authentication_.generation();
  const std::string text = open_contact_book()->own_key_file();
  const std::wstring path = file_chooser_.choose_contact_key_save_path();
  if (path.empty()) return false;
  // Nothing is written for a login that ended while the dialog was open.
  if (!login_still_current(generation)) {
    throw_wrong_route("the login changed while the key file was being saved");
  }
  std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  file.close();
  if (!file) throw std::system_error(std::make_error_code(std::errc::io_error), "write the own key file");
  return true;
}

void CardProductUiSessionController::export_container_selection_for_recipients(
    const std::span<const application::ExportSelectionEntry> entries,
    const std::wstring_view destination_directory, const std::wstring_view folder_name,
    const std::span<const contacts::ContactPublicKey> recipients,
    const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
  std::vector<contacts::ContactPublicKey> chosen;
  // Taken from the list here and then let go of: the export holds only its
  // copies, so a session change can drop the list while the export runs.
  const contacts::ContactIdentity sender = [&] {
    const auto book = open_contact_book();
    if (recipients.empty() || recipients.size() >= backup::kTransferPackageV1Slots) {
      throw std::invalid_argument("a recipient export names 1 to 15 contacts");
    }
    require_same_card();
    if (book->state() == contacts::ContactStoreState::unread) static_cast<void>(book->load());
    // Chosen from the list only: a key the page made up is refused, so what a
    // package is sealed to is always someone the user listed and named.
    chosen.reserve(recipients.size());
    for (const contacts::ContactPublicKey& key : recipients) {
      if (std::ranges::find(chosen, key) != chosen.end() ||
          std::ranges::none_of(book->entries(), [&](const contacts::ContactEntry& entry) {
            return entry.public_key == key;
          })) {
        throw std::invalid_argument("a recipient is not a listed contact or is named twice");
      }
      chosen.push_back(key);
    }
    return book->transfer_identity();
  }();
  volume_session_.export_container_selection_for_recipients(
      entries, destination_directory, folder_name, sender, chosen, cancelled, progress);
}

void CardProductUiSessionController::release_stale_contact_book() noexcept {
  std::shared_ptr<application::CardContactBook> dropped;
  const std::scoped_lock lock(contact_mutex_);
  const bool current = state() == CardProductUiState::authenticated;
  if (!current || contact_book_generation_ != authentication_.generation()) {
    dropped = std::move(contact_book_);
    contact_book_.reset();
  }
  if (!current || pending_contact_key_generation_ != authentication_.generation()) {
    pending_contact_key_.clear();
  }
}

std::wstring CardProductUiSessionController::selected_import_package() {
  const std::scoped_lock lock(import_mutex_);
  // A new attempt keeps only the path, never an earlier verified password.
  verified_import_.reset();
  if (state() != CardProductUiState::authenticated ||
      pending_import_generation_ != authentication_.generation()) {
    pending_import_package_.clear();
    pending_import_restored_name_.clear();
    return {};
  }
  return pending_import_package_;
}

void CardProductUiSessionController::set_selected_import_name(const std::wstring_view name) {
  // The name the import gives its restored entry inside the vault. The
  // package file itself is never renamed: it may sit on read-only media, in
  // a synchronised folder or on someone else's drive, and naming what comes
  // into the app is no reason to change what lies outside it.
  static_cast<void>(volume::resolve_volume_child_path(L'C', L"", name));
  const std::scoped_lock lock(import_mutex_);
  if (state() != CardProductUiState::authenticated || pending_import_package_.empty() ||
      pending_import_generation_ != authentication_.generation()) {
    throw std::logic_error("no selected package belongs to this login");
  }
  pending_import_restored_name_.assign(name);
}

void CardProductUiSessionController::verify_import(
    const std::wstring_view package, const std::span<std::byte> mutable_password_utf8,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  MutableInputWiper wiper(mutable_password_utf8, {});
  {
    const std::scoped_lock lock(import_mutex_);
    verified_import_.reset();
    verified_sender_.reset();
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("importing a package is not available in the current UI state");
  }
  if (package.empty()) throw std::logic_error("no import package has been picked");
  require_same_card();
  auto verified = std::make_unique<VerifiedImport>(package);
  {
    const std::scoped_lock lock(import_mutex_);
    if (package == pending_import_package_ && pending_import_generation_ == authentication_.generation())
      verified->package_file.set_restored_name(pending_import_restored_name_);
  }
  verified->password = consume_password_utf8(mutable_password_utf8);
  verified->generation = authentication_.generation();
  static_cast<void>(verified->package_file.verify(verified->password, cancelled, progress));
  const std::scoped_lock lock(import_mutex_);
  // A login that ended while this ran keeps nothing of it.
  if (state() != CardProductUiState::authenticated ||
      verified->generation != authentication_.generation()) {
    throw std::logic_error("the login changed while the package was verified");
  }
  verified_import_ = std::move(verified);
}

namespace {

// Room for the file system's own structures on top of what the package
// needs, generously, so a container sized at the minimum is not full.
[[nodiscard]] std::uint32_t minimum_mib_for(const std::uint64_t required_bytes) {
  const std::uint64_t padded = required_bytes + required_bytes / 8 + 16ULL * kBytesPerMib;
  const std::uint64_t mib = (padded + kBytesPerMib - 1) / kBytesPerMib;
  return static_cast<std::uint32_t>(
      std::clamp<std::uint64_t>(mib, kContainerCreationMinimumMib, 0xFFFFFFFFULL));
}

}  // namespace

std::uint32_t CardProductUiSessionController::verified_import_minimum_mib() const {
  const std::scoped_lock lock(import_mutex_);
  if (!verified_import_ || state() != CardProductUiState::authenticated ||
      verified_import_->generation != authentication_.generation()) {
    return 0;
  }
  return minimum_mib_for(verified_import_->package_file.required_bytes());
}

std::unique_ptr<CardProductUiSessionController::VerifiedImport>
CardProductUiSessionController::take_verified_import() {
  const std::scoped_lock lock(import_mutex_);
  std::unique_ptr<VerifiedImport> verified = std::move(verified_import_);
  if (!verified || state() != CardProductUiState::authenticated ||
      verified->generation != authentication_.generation()) {
    return {};
  }
  return verified;
}

template <typename Create>
std::uint32_t CardProductUiSessionController::import_into_created(
    std::unique_ptr<VerifiedImport> verified, const std::uint32_t mounted_mib, Create&& create,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  if (!verified || state() != CardProductUiState::authenticated ||
      verified->generation != authentication_.generation()) {
    throw std::logic_error("no verified import belongs to this login");
  }
  if (mounted_mib < minimum_mib_for(verified->package_file.required_bytes())) {
    throw std::invalid_argument("the new container is too small for the package");
  }
  // A transfer package needs this login's contact list for its write pass;
  // asked before anything is created.
  if (verified->package_file.verified_as_transfer()) static_cast<void>(open_contact_book());
  volume_session_.defer_operation_records();
  std::uint32_t id = 0;
  try {
    id = create();
    static_cast<void>(write_verified(*verified, id, L"", cancelled, progress));
  } catch (...) {
    bool undone = true;
    if (id != 0) {
      try {
        volume_session_.discard_new_container(id);
      } catch (...) {
        undone = false;
      }
    }
    volume_session_.settle_operation_records(false);
    if (!undone) throw ImportRollbackIncomplete();
    throw;
  }
  volume_session_.settle_operation_records(true);
  return id;
}

std::wstring CardProductUiSessionController::write_verified(
    VerifiedImport& verified, const std::uint32_t id, const std::wstring_view relative_directory,
    const std::function<bool()>& cancelled,
    const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  if (!verified.package_file.verified_as_transfer()) {
    return volume_session_.write_verified_import(id, relative_directory, verified.package_file,
                                                 verified.password, cancelled, progress);
  }
  // A copy of the identity, owned by this write: the list it came from may
  // be dropped by a session change while the write runs.
  const contacts::ContactIdentity recipient = open_contact_book()->transfer_identity();
  // Taken from the login that verified the package, or not at all.
  if (!login_still_current(verified.generation)) {
    throw std::logic_error("the login changed before the transfer was written");
  }
  return volume_session_.write_verified_transfer_import(id, relative_directory,
                                                        verified.package_file, recipient,
                                                        cancelled, progress);
}

std::optional<CardProductUiSessionController::TransferSenderUi>
CardProductUiSessionController::verify_transfer_import(
    const std::wstring_view package, const std::function<bool()>& cancelled,
    const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  {
    const std::scoped_lock lock(import_mutex_);
    verified_import_.reset();
    verified_sender_.reset();
  }
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("importing a package is not available in the current UI state");
  }
  if (package.empty()) throw std::logic_error("no import package has been picked");
  require_same_card();
  const std::uint64_t generation = authentication_.generation();
  const contacts::ContactIdentity recipient = open_contact_book()->transfer_identity();
  auto verified = std::make_unique<VerifiedImport>(package);
  {
    const std::scoped_lock lock(import_mutex_);
    if (package == pending_import_package_ && pending_import_generation_ == generation)
      verified->package_file.set_restored_name(pending_import_restored_name_);
  }
  verified->generation = generation;
  if (!verified->package_file.verify_transfer(recipient, cancelled, progress)) return std::nullopt;

  // 🔴 The sender is read only now, after its signature verified.
  const contacts::ContactPublicKey sender = verified->package_file.transfer_sender();
  TransferSenderUi result;
  if (contacts::contact_x25519_public_key(sender) == recipient.x25519_public_key()) {
    result.own = true;
  } else {
    const auto book = open_contact_book();
    if (book->state() == contacts::ContactStoreState::unread) static_cast<void>(book->load());
    result.remark = book->remark_for(sender);
    if (!result.remark) result.fingerprint = contacts::format_contact_fingerprint(sender);
  }
  const std::scoped_lock lock(import_mutex_);
  // A login that ended while this ran keeps nothing of it.
  if (!login_still_current(generation)) {
    throw std::logic_error("the login changed while the package was verified");
  }
  verified_import_ = std::move(verified);
  verified_sender_ = sender;
  verified_sender_generation_ = generation;
  return result;
}

application::ContactBookOutcome CardProductUiSessionController::add_transfer_sender(
    const std::string_view remark) {
  contacts::ContactPublicKey sender{};
  std::uint64_t generation{};
  {
    const std::scoped_lock lock(import_mutex_);
    if (!verified_sender_ || verified_sender_generation_ != authentication_.generation()) {
      verified_sender_.reset();
      throw std::logic_error("no verified sender belongs to this login");
    }
    sender = *verified_sender_;
    generation = verified_sender_generation_;
  }
  const std::string key_file = contacts::encode_contact_key_file(sender);
  const application::ContactBookOutcome outcome =
      write_contact_book([&](application::CardContactBook& book) {
        // Only into the list of the login that verified the sender.
        if (!login_still_current(generation)) {
          throw_wrong_route("the login changed before the sender was added");
        }
        return book.add(key_file, remark);
      });
  if (outcome.change == application::ContactBookChange::done) {
    const std::scoped_lock lock(import_mutex_);
    if (verified_sender_ == sender) verified_sender_.reset();
  }
  return outcome;
}

std::wstring CardProductUiSessionController::import_verified_into_container(
    std::unique_ptr<VerifiedImport> verified, const std::uint32_t id,
    const std::wstring_view relative_directory, const std::function<bool()>& cancelled,
    const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  if (!verified || state() != CardProductUiState::authenticated ||
      verified->generation != authentication_.generation()) {
    throw std::logic_error("no verified import belongs to this login");
  }
  require_same_card();
  return write_verified(*verified, id, relative_directory, cancelled, progress);
}

std::uint32_t CardProductUiSessionController::import_into_new_container(
    std::unique_ptr<VerifiedImport> verified, const std::wstring_view name,
    const std::wstring_view directory, const std::uint32_t size_mib,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  return import_into_created(std::move(verified), size_mib,
                             [&] { return create_container(name, directory, size_mib); },
                             cancelled, progress);
}

std::uint32_t CardProductUiSessionController::import_into_new_container_b(
    std::unique_ptr<VerifiedImport> verified, const std::wstring_view name,
    const std::wstring_view directory, const std::uint32_t outer_size_mib,
    const std::uint32_t hidden_size_mib, const std::span<std::byte> mutable_second_password_utf8,
    const std::span<std::byte> mutable_second_confirmation_utf8,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  MutableInputWiper wiper(mutable_second_password_utf8, mutable_second_confirmation_utf8);
  // The import lands in the volume the session mounts for a new container of
  // this kind, which is the inner one, so that is the size that must fit.
  return import_into_created(
      std::move(verified), hidden_size_mib,
      [&] {
        return create_container_b(name, directory, outer_size_mib, hidden_size_mib,
                                  mutable_second_password_utf8, mutable_second_confirmation_utf8);
      },
      cancelled, progress);
}

std::wstring CardProductUiSessionController::import_package_into_container(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view package, const std::span<std::byte> mutable_password_utf8,
    const std::function<bool()>& cancelled,
    const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) {
  MutableInputWiper wiper(mutable_password_utf8, {});
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("importing a package is not available in the current UI state");
  }
  if (package.empty()) throw std::logic_error("no import package has been picked");
  require_same_card();
  security::SecureBuffer password = consume_password_utf8(mutable_password_utf8);
  SecureBufferGuard password_guard(password);
  std::wstring restored_name;
  {
    const std::scoped_lock lock(import_mutex_);
    if (package == pending_import_package_ && pending_import_generation_ == authentication_.generation())
      restored_name = pending_import_restored_name_;
  }
  if (!restored_name.empty()) {
    backup::ImportPackageV1 selected(package);
    selected.set_restored_name(restored_name);
    static_cast<void>(selected.verify(password, cancelled, progress));
    return volume_session_.write_verified_import(id, relative_directory, selected, password, cancelled, progress);
  }
  return volume_session_.import_package_into_container(id, relative_directory, package,
                                                       password, cancelled, progress);
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

void CardProductUiSessionController::import_external_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view absolute_source,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route(
        "importing an external entry is not available in the current UI state");
  }
  volume_session_.import_external_entry(id, relative_directory,
                                        absolute_source, cancelled, progress);
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

void CardProductUiSessionController::unmount_batch_item(const std::uint32_t id, const bool force) {
  if (state() != CardProductUiState::authenticated)
    throw_wrong_route("unmounting a container is not available in the current UI state");
  volume_session_.unmount_batch_item(id, force);
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
      return activation_state() == licensing::ActivationState::active
                 ? CardProductUiState::initialization_required
                 : CardProductUiState::activation_required;
    case application::CardEntryRoute::login_required:
      return activation_state() == licensing::ActivationState::active
                 ? CardProductUiState::login_required
                 : CardProductUiState::activation_required;
    case application::CardEntryRoute::unavailable:
    default:
      return CardProductUiState::unavailable;
  }
}

std::optional<std::uint32_t> CardProductUiSessionController::activation_card()
    const noexcept {
  return authentication_.authenticated() ? authenticated_card_
                                         : entry_.current_card_identity();
}

licensing::ActivationState CardProductUiSessionController::activation_state()
    const noexcept {
  if (activation_ == nullptr) return licensing::ActivationState::active;
  const std::optional<std::uint32_t> card = activation_card();
  // A card without an identity cannot hold a grant.
  if (!card) return licensing::ActivationState::missing;
  return activation_->state(*card);
}

std::optional<std::int64_t> CardProductUiSessionController::activation_expiry()
    const noexcept {
  const std::optional<std::uint32_t> card = activation_card();
  if (activation_ == nullptr || !card) return std::nullopt;
  return activation_->expiry(*card);
}

application::ActivationInstallResult
CardProductUiSessionController::submit_activation_code(const std::string_view code) {
  const CardProductUiState current = state();
  if (activation_ == nullptr ||
      (current != CardProductUiState::activation_required &&
       current != CardProductUiState::authenticated)) {
    throw_wrong_route("activation is not available in the current UI state");
  }
  const std::optional<std::uint32_t> card = activation_card();
  if (!card) throw_wrong_route("the current card has no identity");
  // Only a renewal inside a session is rechecked; before login the card was
  // just read and no session exists to compare it with.
  if (current == CardProductUiState::authenticated) require_same_card();
  const auto result = activation_->install(*card, code);
  USB_VAULT_TRACE("激活", "提交激活码", "结果 " + std::to_string(static_cast<int>(result)));
  return result;
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
    // Capacity and drive letter share one rule: both describe an attached
    // volume, so both are present exactly when the container is mounted. The
    // one exception is a mounted row the driver could not report, which keeps
    // its letter but has no capacity to show.
    if (status.id == 0 || status.name.empty() ||
        (status.mounted &&
         (status.drive_letter < L'A' || status.drive_letter > L'Z' ||
          (status.size_bytes == 0) != status.volume_unavailable)) ||
        (!status.mounted &&
         (status.drive_letter != wchar_t{} || status.size_bytes != 0 ||
          status.volume_unavailable))) {
      throw std::runtime_error("container status is inconsistent");
    }
    result.push_back({.id = status.id,
                      .name = status.name,
                      .mounted = status.mounted,
                      .size_mib = displayable_size_mib(status.size_bytes),
                      .drive_letter = status.drive_letter,
                      .partition = status.partition, .conversion_state=status.conversion_state,
                      .volume_unavailable = status.volume_unavailable});
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
  require_same_card();
  // A mount that outlives its key material would hold a drive letter nothing
  // can release, so an attached volume stops the operation before the card is
  // written at all.
  volume_session_.unmount_active();
  if (volume_session_.mounted()) {
    throw std::system_error(
        card::make_error_code(card::CardError::invalid_operation),
        "the card cannot be reset while a volume is still mounted");
  }

  try {
    entry_.reset_card_record(authentication_);
  } catch (...) {
    volume_session_.release_session_secrets();
    volume_session_.clear_operation_history();
    authentication_.clear_authentication();
    throw;
  }

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
bool CardProductUiSessionController::hide_new_file_containers() {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("file visibility requires authentication");
  return volume_session_.hide_new_file_containers();
}
void CardProductUiSessionController::set_hide_new_file_containers(bool value) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("file visibility requires authentication");
  volume_session_.set_hide_new_file_containers(value);
}
bool CardProductUiSessionController::container_file_hidden(std::uint32_t id) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("file visibility requires authentication");
  return volume_session_.container_file_hidden(id);
}
void CardProductUiSessionController::set_container_file_hidden(std::uint32_t id, bool hidden) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("file visibility requires authentication");
  volume_session_.set_container_file_hidden(id, hidden);
}
unsigned int CardProductUiSessionController::container_extension_preference() {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  const auto value = volume_session_.container_extension_preference();
  if (value > 3) throw std::invalid_argument("invalid container extension preference");
  return value;
}
void CardProductUiSessionController::set_container_extension_preference(unsigned int value) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("preference requires authentication");
  if (value > 3) throw std::invalid_argument("invalid container extension preference");
  volume_session_.set_container_extension_preference(value);
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

void CardProductUiSessionController::rename_container(std::uint32_t id, std::wstring_view name) {
  if (state() != CardProductUiState::authenticated) throw_wrong_route("renaming requires authentication");
  volume_session_.rename_container(id, name);
}

void CardProductUiSessionController::set_operation_logging_enabled(bool enabled) {
  if (state() != CardProductUiState::authenticated) {
    throw_wrong_route("operation logging preference requires authentication");
  }
  volume_session_.set_operation_logging_enabled(enabled);
}

}  // namespace usb_vault::ui

namespace usb_vault::ui {
bool CardProductUiSessionController::can_login_reset() const noexcept {
  return !authentication_.authenticated() && !login_reset_completed_ &&
         entry_.can_login_reset();
}
LoginResetPreparation CardProductUiSessionController::prepare_login_reset() {
  cancel_login_reset();
  if (!can_login_reset()) throw std::logic_error("login reset unavailable");
  LoginResetPreparation result;
  result.ticket = entry_.prepare_login_reset();
  try {
    if (!reset_mount_access_) throw std::runtime_error("mount query unavailable");
    reset_mounts_ = reset_mount_access_->mounted();
    if (reset_mounts_.size() > 26) throw std::runtime_error("invalid mount list");
    for (const auto& mount : reset_mounts_) {
      if (mount.drive_letter < L'A' || mount.drive_letter > L'Z')
        throw std::runtime_error("invalid mount drive");
      if (!result.drives.empty()) result.drives += L", ";
      result.drives += mount.drive_letter; result.drives += L":";
    }
    result.mounts_known = reset_mounts_known_ = true;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("ui/card_product_ui_session_controller", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    reset_mounts_.clear(); result.drives.clear(); reset_mounts_known_ = false;
  }
  return result;
}
void CardProductUiSessionController::cancel_login_reset() noexcept {
  entry_.cancel_login_reset(); reset_mounts_.clear(); reset_mounts_known_ = false;
}
bool CardProductUiSessionController::login_reset_pending(std::uint32_t ticket) const noexcept {
  return can_login_reset() && entry_.login_reset_pending(ticket);
}
void CardProductUiSessionController::force_login_reset_mounts(std::uint32_t ticket, const std::function<bool()>& interrupted) {
  if (!login_reset_pending(ticket) || !reset_mount_access_ || !reset_mounts_known_ || reset_mounts_.empty())
    throw std::logic_error("no confirmed mount targets");
  // Never extend the saved set to newly mounted volumes. Reused letters fail.
  for (const auto& expected : reset_mounts_) {
    if (interrupted && interrupted()) throw std::runtime_error("reset interrupted");
    const auto current = reset_mount_access_->mounted();
    const auto found = std::ranges::find(current, expected.drive_letter, &volume::MountIdentity::drive_letter);
    if (found == current.end()) continue;
    if (*found != expected) throw std::runtime_error("mount identity changed");
    reset_mount_access_->force_unmount(expected);
  }
}
void CardProductUiSessionController::confirm_login_reset(std::uint32_t ticket) {
  if (!login_reset_pending(ticket)) { cancel_login_reset(); throw std::logic_error("confirmation expired"); }
  reset_mounts_.clear(); reset_mounts_known_ = false;
  try {
    entry_.confirm_login_reset(ticket);
  } catch (...) {
    volume_session_.release_session_secrets();
    volume_session_.clear_operation_history();
    authentication_.clear_authentication();
    throw;
  }
  login_reset_completed_ = true;
  volume_session_.release_session_secrets();
  volume_session_.clear_operation_history();
  authentication_.clear_authentication();
}
}  // namespace usb_vault::ui
