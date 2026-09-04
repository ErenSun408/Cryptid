#include "usb_vault/application/card_login_mount_session.h"

#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "volume/mount_request.h"

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace usb_vault::application {
namespace {

class ProductionCardLoginMountSessionRuntime final
    : public detail::CardLoginMountSessionRuntime {
 public:
  explicit ProductionCardLoginMountSessionRuntime(
      const volume::VolumeProvider& provider,
      std::filesystem::path volume_creator_helper_path)
      : provider_(provider),
        creator_(std::move(volume_creator_helper_path)) {}

  [[nodiscard]] MountSessionResult mount_authenticated(
      const ContainerLayout& layout,
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& root_secret) override {
    return mount_authenticated_card_session(
        provider_, layout, login_password_utf8, root_secret);
  }

  [[nodiscard]] volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    return volume::unmount_with_retry(provider_, drive_letter);
  }

  [[nodiscard]] volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    return volume::unmount_with_forced_escalation(provider_, drive_letter);
  }

  [[nodiscard]] bool container_exists(
      const std::wstring_view path) override {
    return container_size_if_present(path).has_value();
  }

  [[nodiscard]] std::optional<std::uint64_t> container_size_if_present(
      const std::wstring_view path) override {
    volume::detail::validate_volume_path(path);
    const std::wstring owned_path(path);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(owned_path.c_str(), GetFileExInfoStandard,
                               &attributes) == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return std::nullopt;
      }
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "inspect container status path");
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      throw std::invalid_argument(
          "container status path refers to a directory");
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::invalid_argument(
          "container status path must not be a reparse point");
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    return size.QuadPart;
  }

  [[nodiscard]] std::optional<std::uint64_t> mounted_volume_size(
      const wchar_t drive_letter) override {
    for (const volume::MountedVolume& mounted : provider_.mounted_volumes()) {
      if (mounted.drive_letter == drive_letter) return mounted.disk_length;
    }
    return std::nullopt;
  }

  [[nodiscard]] security::SecureBuffer derive_container_password(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& root_secret,
      const security::KdfV1Salt& salt) override {
    return security::derive_volume_password_v1(password_input, root_secret,
                                                salt);
  }

  void create_standard(
      const std::wstring_view target_path, const std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) override {
    creator_.create_standard(target_path, size_bytes, volume_password);
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) override {
    creator_.create_with_hidden(target_path, outer_size_bytes,
                                hidden_size_bytes, outer_password,
                                hidden_password);
  }

  void publish_created(const std::wstring_view staging_path,
                       const std::wstring_view target_path) override {
    const std::wstring staging(staging_path);
    const std::wstring target(target_path);
    if (::MoveFileExW(staging.c_str(), target.c_str(),
                      MOVEFILE_WRITE_THROUGH) == FALSE) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "publish created container");
    }
  }

  void remove_incomplete(const std::wstring_view staging_path) override {
    const std::wstring staging(staging_path);
    if (::DeleteFileW(staging.c_str()) != FALSE) return;
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "remove incomplete container");
    }
  }

  [[nodiscard]] volume::MountResult mount_created(
      const std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) override {
    return provider_.mount_file(path, volume_password, options);
  }

  [[nodiscard]] wchar_t allocate_drive_letter() override {
    const DWORD drives = ::GetLogicalDrives();
    if (drives == 0) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "inspect available drive letters");
    }
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
      const DWORD mask = 1UL << static_cast<DWORD>(letter - L'A');
      if ((drives & mask) == 0) return letter;
    }
    throw std::system_error(
        std::make_error_code(std::errc::no_space_on_device),
        "no free drive letter is available for a container mount");
  }

  void notify_mounted(const wchar_t drive_letter) override {
    provider_.notify_mounted(drive_letter);
  }

 private:
  const volume::VolumeProvider& provider_;
  volume_creation::VolumeCreator creator_;
};

}  // namespace

ContainerCreationCleanupError::ContainerCreationCleanupError(
    std::exception_ptr operation_failure,
    std::exception_ptr cleanup_failure)
    : std::runtime_error(
          "container creation failed and incomplete-file cleanup failed"),
      operation_failure_(std::move(operation_failure)),
      cleanup_failure_(std::move(cleanup_failure)) {}

const std::exception_ptr&
ContainerCreationCleanupError::operation_failure() const noexcept {
  return operation_failure_;
}

const std::exception_ptr&
ContainerCreationCleanupError::cleanup_failure() const noexcept {
  return cleanup_failure_;
}

struct CardLoginMountSession::Impl final {
  Impl(CardAuthenticationSession& authentication,
       const volume::VolumeProvider& provider,
       ContainerLayout layout,
       std::filesystem::path volume_creator_helper_path,
       LoginMountPolicy login_mount_policy)
      : runtime(provider, std::move(volume_creator_helper_path)),
        core(authentication, runtime, std::move(layout),
             login_mount_policy) {}

  ProductionCardLoginMountSessionRuntime runtime;
  detail::CardLoginMountSessionCore core;
};

CardLoginMountSession::CardLoginMountSession(
    CardAuthenticationSession& authentication,
    const volume::VolumeProvider& provider,
    ContainerLayout layout,
    std::filesystem::path volume_creator_helper_path,
    const LoginMountPolicy login_mount_policy)
    : impl_(std::make_unique<Impl>(authentication, provider,
                                   std::move(layout),
                                   std::move(volume_creator_helper_path),
                                   login_mount_policy)) {}

CardLoginMountSession::~CardLoginMountSession() noexcept = default;

void CardLoginMountSession::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  impl_->core.consume_authenticated_login(
      std::move(root_secret), std::move(login_password_utf8));
}

void CardLoginMountSession::create_container_a(
    const std::uint64_t size_bytes) {
  impl_->core.create_container_a(size_bytes);
}

std::uint32_t CardLoginMountSession::create_container(
    const std::wstring_view path, const std::uint64_t size_bytes) {
  return impl_->core.create_container(path, size_bytes);
}

std::uint32_t CardLoginMountSession::create_container_b(
    const std::wstring_view path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t hidden_size_bytes,
    const security::SecureBuffer& decoy_password_utf8) {
  return impl_->core.create_container_b(path, outer_size_bytes,
                                        hidden_size_bytes,
                                        decoy_password_utf8);
}

security::SecureBuffer CardLoginMountSession::copy_session_password() {
  return impl_->core.copy_session_password();
}

std::uint32_t CardLoginMountSession::mount_container(
    const std::wstring_view path) {
  return impl_->core.mount_container(path);
}

void CardLoginMountSession::mount_adopted_container(const std::uint32_t id) {
  impl_->core.mount_adopted_container(id);
}

void CardLoginMountSession::unmount_container(const std::uint32_t id) {
  impl_->core.unmount_container(id);
}

std::vector<ContainerStatus> CardLoginMountSession::container_statuses() {
  return impl_->core.container_statuses();
}

ContainerAStatus CardLoginMountSession::container_a_status() {
  return impl_->core.container_a_status();
}

std::vector<OperationEvent> CardLoginMountSession::operation_history() {
  return impl_->core.operation_history();
}

void CardLoginMountSession::clear_operation_history() noexcept {
  impl_->core.clear_operation_history();
}

void CardLoginMountSession::release_session_secrets() noexcept {
  impl_->core.release_session_secrets();
}

void CardLoginMountSession::unmount_active() { impl_->core.unmount_active(); }

bool CardLoginMountSession::force_unmount_active() {
  return impl_->core.force_unmount_active();
}

bool CardLoginMountSession::mounted() const noexcept {
  return impl_->core.mounted();
}

namespace detail {
namespace {

// SecureBuffer is deliberately not copyable, so every retained secret is
// duplicated explicitly and cleared on its own path.
[[nodiscard]] security::SecureBuffer duplicate_secret(
    const security::SecureBuffer& source) {
  security::SecureBuffer result(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    result.bytes()[index] = source.bytes()[index];
  }
  return result;
}

[[nodiscard]] security::SecureBuffer container_a_password_input() {
  security::SecureBuffer input(kContainerAPasswordInput.size());
  std::ranges::copy(kContainerAPasswordInput, input.bytes().begin());
  return input;
}

[[nodiscard]] std::wstring staging_path_for(
    const std::wstring_view container_path) {
  std::wstring path(container_path);
  path += L".preparing";
  return path;
}

[[nodiscard]] std::wstring file_name_of(
    const std::wstring_view container_path) {
  const std::size_t separator = container_path.find_last_of(L"\\/");
  const std::wstring_view name =
      separator == std::wstring_view::npos
          ? container_path
          : container_path.substr(separator + 1);
  return std::wstring(name);
}

[[noreturn]] void cleanup_incomplete_container(
    CardLoginMountSessionRuntime& runtime,
    const std::wstring_view staging_path,
    const std::exception_ptr& operation_failure) {
  try {
    runtime.remove_incomplete(staging_path);
  } catch (...) {
    throw ContainerCreationCleanupError(operation_failure,
                                        std::current_exception());
  }
  std::rethrow_exception(operation_failure);
}

[[nodiscard]] SessionContainer* find_by_id(
    CardLoginMountSessionState& state, const std::uint32_t id) noexcept {
  for (SessionContainer& container : state.containers) {
    if (container.id == id) return &container;
  }
  return nullptr;
}

[[nodiscard]] SessionContainer* find_by_path(
    CardLoginMountSessionState& state,
    const std::wstring_view path) noexcept {
  for (SessionContainer& container : state.containers) {
    if (container.path == path) return &container;
  }
  return nullptr;
}

// Adoption is idempotent so a login mount, a residual-mount record and an
// explicit mount all converge on one entry per container path.
SessionContainer& adopt(CardLoginMountSessionState& state,
                        const std::wstring_view path,
                        const wchar_t drive_letter, const bool mounted,
                        const bool release_first = false) {
  if (SessionContainer* const existing = find_by_path(state, path)) {
    existing->drive_letter = drive_letter;
    existing->mounted = mounted;
    existing->release_first = existing->release_first || release_first;
    return *existing;
  }
  state.containers.push_back({.id = state.next_container_id,
                              .path = std::wstring(path),
                              .name = file_name_of(path),
                              .drive_letter = drive_letter,
                              .mounted = mounted,
                              .release_first = release_first});
  ++state.next_container_id;
  return state.containers.back();
}

void record_residual_mount(const ContainerLayout& layout,
                           CardLoginMountSessionState& state,
                           const wchar_t drive_letter) noexcept {
  try {
    if (drive_letter == layout.container_a.drive_letter) {
      static_cast<void>(adopt(state, layout.container_a.path, drive_letter,
                              true));
    } else if (drive_letter == layout.container_b.drive_letter) {
      static_cast<void>(adopt(state, layout.container_b.path, drive_letter,
                              true, true));
    }
  } catch (...) {
    // A residual drive letter that cannot be recorded is still held by this
    // session; the unified cleanup path releases it by drive letter.
  }
}

}  // namespace

CardLoginMountSessionCore::CardLoginMountSessionCore(
    CardAuthenticationSession& authentication,
    CardLoginMountSessionRuntime& runtime,
    ContainerLayout layout,
    const LoginMountPolicy login_mount_policy)
    : authentication_(authentication),
      runtime_(runtime),
      layout_(std::move(layout)),
      login_mount_policy_(login_mount_policy) {}

void CardLoginMountSessionCore::record_operation(
    const OperationKind kind,
    const OperationOutcome outcome,
    const std::wstring_view container_name,
    const wchar_t drive_letter) noexcept {
  try {
    const auto completed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
    OperationEvent event{
        .sequence = state_.next_operation_sequence,
        .completed_at_unix_ms = completed.count(),
        .kind = kind,
        .outcome = outcome,
        .container_name = std::wstring(container_name),
        .drive_letter = drive_letter,
    };

    const bool full = state_.operation_count == state_.operation_events.size();
    const std::size_t slot = (state_.operation_start + state_.operation_count) %
                             state_.operation_events.size();
    const std::size_t destination = full ? state_.operation_start : slot;
    state_.operation_events[destination] = std::move(event);
    if (full) {
      state_.operation_start =
          (state_.operation_start + 1) % state_.operation_events.size();
    } else {
      ++state_.operation_count;
    }
    ++state_.next_operation_sequence;
  } catch (...) {
    // History is informational. Allocation or clock failure must never change
    // the result of the security or volume operation being recorded.
  }
}

std::vector<OperationEvent> CardLoginMountSessionCore::operation_history() {
  std::vector<OperationEvent> result;
  result.reserve(state_.operation_count);
  for (std::size_t index = 0; index < state_.operation_count; ++index) {
    const std::size_t slot =
        (state_.operation_start + index) % state_.operation_events.size();
    result.push_back(state_.operation_events[slot]);
  }
  return result;
}

void CardLoginMountSessionCore::clear_operation_history() noexcept {
  for (OperationEvent& event : state_.operation_events) {
    event.container_name.clear();
    event.sequence = 0;
    event.completed_at_unix_ms = 0;
    event.kind = OperationKind::session_opened;
    event.outcome = OperationOutcome::succeeded;
    event.drive_letter = wchar_t{};
  }
  state_.operation_start = 0;
  state_.operation_count = 0;
  state_.next_operation_sequence = 1;
}

void CardLoginMountSessionCore::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  if (state_.mounted() || authentication_.authenticated()) {
    root_secret.clear();
    login_password_utf8.clear();
    throw std::logic_error(
        "authenticated card mount session is already active");
  }

  try {
    authentication_.establish(std::move(root_secret));
    clear_operation_history();
    record_operation(OperationKind::session_opened,
                     OperationOutcome::succeeded);
    state_.hidden_container_password = security::SecureBuffer();
    // Retained for the card-record rewrite and for the shared hidden-volume
    // derivation. It is the password itself rather than a derivative because
    // rewrapping an envelope needs the password as key material, and the
    // record has no third slot to spare.
    state_.login_password = duplicate_secret(login_password_utf8);
    if (runtime_.container_exists(layout_.container_b.path)) {
      // The layout path predates user-chosen locations and is adopted only
      // when a file is really sitting there. Its volume password is derived on
      // first use like that of every other hidden-volume container.
      static_cast<void>(adopt(state_, layout_.container_b.path, wchar_t{},
                              false, true));
    }
    if (runtime_.container_exists(layout_.container_a.path)) {
      static_cast<void>(adopt(state_, layout_.container_a.path, wchar_t{},
                              false));
    }
    if (login_mount_policy_ == LoginMountPolicy::manual) {
      // The login input only ever existed for the synchronous mount call, so
      // the manual policy releases it as soon as authentication is volatile.
      login_password_utf8.clear();
      return;
    }
    const MountSessionResult result = runtime_.mount_authenticated(
        layout_, login_password_utf8, authentication_.root_secret_);
    if (result.container_a.has_value()) {
      static_cast<void>(adopt(state_, layout_.container_a.path,
                              layout_.container_a.drive_letter, true));
    }
    if (result.container_b.has_value()) {
      static_cast<void>(adopt(state_, layout_.container_b.path,
                              layout_.container_b.drive_letter, true, true));
    }
    login_password_utf8.clear();
  } catch (const MountRollbackError& error) {
    record_residual_mount(layout_, state_, error.residual_drive_letter());
    login_password_utf8.clear();
    throw;
  } catch (...) {
    login_password_utf8.clear();
    throw;
  }
}

void CardLoginMountSessionCore::create_container_a(
    const std::uint64_t size_bytes) {
  static_cast<void>(create_container(layout_.container_a.path, size_bytes));
}

security::SecureBuffer CardLoginMountSessionCore::derive_standard_password() {
  security::SecureBuffer fixed_input = container_a_password_input();
  try {
    security::SecureBuffer volume_password =
        runtime_.derive_container_password(
            fixed_input, authentication_.root_secret_,
            layout_.container_a.salt);
    fixed_input.clear();
    return volume_password;
  } catch (...) {
    fixed_input.clear();
    throw;
  }
}

const security::SecureBuffer&
CardLoginMountSessionCore::hidden_container_password() {
  if (!state_.hidden_container_password.empty()) {
    return state_.hidden_container_password;
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  // One derivation opens every hidden-volume container: they share the salt
  // and the pair of passwords, so the login input is run through the KDF once
  // and the result serves the rest of the session.
  state_.hidden_container_password = runtime_.derive_container_password(
      state_.login_password, authentication_.root_secret_,
      layout_.container_b.salt);
  return state_.hidden_container_password;
}

wchar_t CardLoginMountSessionCore::reserve_drive_letter() {
  const wchar_t letter = runtime_.allocate_drive_letter();
  if (letter < L'A' || letter > L'Z') {
    throw std::runtime_error("allocated drive letter is out of range");
  }
  for (const SessionContainer& container : state_.containers) {
    if (container.mounted && container.drive_letter == letter) {
      throw std::runtime_error(
          "allocated drive letter is already held by this session");
    }
  }
  return letter;
}

std::uint32_t CardLoginMountSessionCore::create_container(
    const std::wstring_view path, const std::uint64_t size_bytes) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "container creation requires an authenticated card session");
  }
  if (size_bytes == 0 || size_bytes % 512ULL != 0) {
    throw std::invalid_argument(
        "container size must be a non-zero multiple of 512 bytes");
  }
  if (find_by_path(state_, path) != nullptr) {
    throw std::logic_error("container path is already adopted");
  }

  const std::wstring staging_path = staging_path_for(path);
  if (runtime_.container_exists(path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container creation target already exists");
  }

  const std::wstring container_name = file_name_of(path);
  security::SecureBuffer volume_password;
  bool staging_owned = false;
  bool published = false;
  bool mounted = false;
  try {
    volume_password = derive_standard_password();
    runtime_.create_standard(staging_path, size_bytes, volume_password);
    staging_owned = true;
    runtime_.publish_created(staging_path, path);
    published = true;
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);

    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(path,
                                             volume_password,
                                             {.drive_letter = drive_letter,
                                              .prf = volume::Pkcs5Prf::sha512,
                                              .pim = 0,
                                              .read_only = false,
                                              .notify_shell = false}));
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    const std::uint32_t id = adopt(state_, path, drive_letter, true).id;
    volume_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    volume_password.clear();
    if (!published) {
      record_operation(OperationKind::container_created,
                       OperationOutcome::failed,
                       container_name);
    } else if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::create_container_b(
    const std::wstring_view path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t hidden_size_bytes,
    const security::SecureBuffer& decoy_password_utf8) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "container creation requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  if (outer_size_bytes == 0 || outer_size_bytes % 512ULL != 0 ||
      hidden_size_bytes % 512ULL != 0 ||
      hidden_size_bytes >= outer_size_bytes) {
    throw std::invalid_argument(
        "outer and hidden sizes must be aligned and leave room for the outer "
        "volume");
  }
  if (decoy_password_utf8.size() == 0) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }

  if (find_by_path(state_, path) != nullptr) {
    throw std::logic_error("container path is already adopted");
  }
  const std::wstring staging_path = staging_path_for(path);
  if (runtime_.container_exists(path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container creation target already exists");
  }
  const std::wstring container_name = file_name_of(path);

  // The password the caller is logged in under opens the hidden volume; the
  // password set here opens the outer one. The session never decides which of
  // the two the caller "really" uses. Both derivations are shared by every
  // container of this kind, so a second one costs no extra key material.
  security::SecureBuffer outer_password;
  bool staging_owned = false;
  bool published = false;
  bool mounted = false;
  try {
    outer_password =
        runtime_.derive_container_password(decoy_password_utf8,
                                           authentication_.root_secret_,
                                           layout_.container_b.salt);
    const security::SecureBuffer& hidden_password =
        hidden_container_password();
    runtime_.create_with_hidden(staging_path,
                                outer_size_bytes,
                                hidden_size_bytes,
                                outer_password,
                                hidden_password);
    staging_owned = true;
    runtime_.publish_created(staging_path, path);
    published = true;
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);

    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(path,
                                             hidden_password,
                                             {.drive_letter = drive_letter,
                                              .prf = volume::Pkcs5Prf::sha512,
                                              .pim = 0,
                                              .read_only = false,
                                              .notify_shell = false}));
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    const std::uint32_t id = adopt(state_, path, drive_letter, true, true).id;
    outer_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    outer_password.clear();
    if (!published) {
      record_operation(OperationKind::container_created,
                       OperationOutcome::failed,
                       container_name);
    } else if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

security::SecureBuffer CardLoginMountSessionCore::copy_session_password() {
  if (!authentication_.authenticated()) return security::SecureBuffer();
  return duplicate_secret(state_.login_password);
}

void CardLoginMountSessionCore::release_session_secrets() noexcept {
  state_.hidden_container_password = security::SecureBuffer();
  state_.login_password = security::SecureBuffer();
}

void CardLoginMountSessionCore::mount_adopted_container(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting a container requires an authenticated card session");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (entry->mounted) return;
  const std::wstring container_name = entry->name;
  security::SecureBuffer standard_password;
  bool mounted = false;
  try {
    if (!runtime_.container_exists(entry->path)) {
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "adopted container file is no longer present");
    }

    const bool hidden_volume = entry->release_first;
    if (hidden_volume && state_.login_password.empty()) {
      throw std::logic_error(
          "this container can only be opened by the login that captured it");
    }
    if (!hidden_volume) standard_password = derive_standard_password();
    const security::SecureBuffer& volume_password =
        hidden_volume ? hidden_container_password() : standard_password;
    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(entry->path,
                                             volume_password,
                                             {.drive_letter = drive_letter,
                                              .prf = volume::Pkcs5Prf::sha512,
                                              .pim = 0,
                                              .read_only = false,
                                              .notify_shell = false}));
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    standard_password.clear();
    entry->drive_letter = drive_letter;
    entry->mounted = true;
    runtime_.notify_mounted(drive_letter);
  } catch (...) {
    standard_password.clear();
    if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::mount_container(
    const std::wstring_view path) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting a container requires an authenticated card session");
  }
  const std::wstring container_name = file_name_of(path);
  if (const SessionContainer* const adopted = find_by_path(state_, path);
      adopted != nullptr && adopted->mounted) {
    throw std::logic_error("container is already mounted");
  }
  security::SecureBuffer volume_password;
  bool mounted = false;
  bool hidden_volume = false;
  try {
    if (!runtime_.container_exists(path)) {
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "container file is unavailable");
    }
    volume_password = derive_standard_password();
    const wchar_t drive_letter = reserve_drive_letter();
    const volume::MountOptions options{.drive_letter = drive_letter,
                                       .prf = volume::Pkcs5Prf::sha512,
                                       .pim = 0,
                                       .read_only = false,
                                       .notify_shell = false};
    try {
      static_cast<void>(runtime_.mount_created(path, volume_password,
                                               options));
    } catch (const std::system_error& error) {
      // The user selects a file, never a container class, and the page must
      // not offer that choice either. A file the standard derivation cannot
      // open is either built from the login input or not a container of ours
      // at all, and the two stay indistinguishable until the second
      // derivation has been tried.
      if (error.code() != volume::make_error_code(
                              volume::DriverError::authentication_failed) ||
          state_.login_password.empty()) {
        throw;
      }
      volume_password.clear();
      static_cast<void>(runtime_.mount_created(
          path, hidden_container_password(), options));
      hidden_volume = true;
    }
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    const std::uint32_t id =
        adopt(state_, path, drive_letter, true, hidden_volume).id;
    volume_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    volume_password.clear();
    if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    throw;
  }
}

void CardLoginMountSessionCore::unmount_container(const std::uint32_t id) {
  for (SessionContainer& container : state_.containers) {
    if (container.id != id) continue;
    if (!container.mounted) return;
    const wchar_t drive_letter = container.drive_letter;
    try {
      static_cast<void>(runtime_.unmount(drive_letter));
      container.mounted = false;
      container.drive_letter = wchar_t{};
      record_operation(OperationKind::container_unmounted,
                       OperationOutcome::succeeded,
                       container.name,
                       drive_letter);
      return;
    } catch (...) {
      record_operation(OperationKind::container_unmounted,
                       OperationOutcome::failed,
                       container.name,
                       drive_letter);
      throw;
    }
  }
  throw std::invalid_argument("no container is adopted under that identifier");
}

std::vector<ContainerStatus> CardLoginMountSessionCore::container_statuses() {
  std::vector<ContainerStatus> result;
  result.reserve(state_.containers.size());
  for (const SessionContainer& container : state_.containers) {
    if (!container.mounted) {
      // An unmounted container has no capacity to report. The backing file
      // size is not a volume size, and for a container holding two volumes
      // which of them would appear is not decided until it is mounted.
      result.push_back({.id = container.id,
                        .name = container.name,
                        .mounted = false,
                        .size_bytes = 0,
                        .drive_letter = wchar_t{}});
      continue;
    }
    // The driver is the only authority on an attached volume: it alone knows
    // whether the hidden or the outer volume of a two-volume container is the
    // one currently on that drive.
    const std::optional<std::uint64_t> size =
        runtime_.mounted_volume_size(container.drive_letter);
    if (!size.has_value() || *size == 0) {
      throw std::runtime_error(
          "mounted container has no volume on its drive letter");
    }
    result.push_back({.id = container.id,
                      .name = container.name,
                      .mounted = true,
                      .size_bytes = *size,
                      .drive_letter = container.drive_letter});
  }
  return result;
}

ContainerAStatus CardLoginMountSessionCore::container_a_status() {
  const std::optional<std::uint64_t> size =
      runtime_.container_size_if_present(layout_.container_a.path);
  const SessionContainer* const adopted =
      find_by_path(state_, layout_.container_a.path);
  const bool mounted = adopted != nullptr && adopted->mounted;
  if (!size.has_value()) {
    if (mounted) {
      throw std::runtime_error(
          "mounted container A backing file is unavailable");
    }
    return {};
  }
  return {.exists = true,
          .mounted = mounted,
          .size_bytes = *size,
          .drive_letter = mounted ? adopted->drive_letter : wchar_t{}};
}

void CardLoginMountSessionCore::unmount_active() {
  // The B-before-A order required after authentication still holds: the
  // container with a hidden volume is released first, then every other one.
  for (const bool release_first : {true, false}) {
    for (SessionContainer& container : state_.containers) {
      if (!container.mounted || container.release_first != release_first) {
        continue;
      }
      const wchar_t drive_letter = container.drive_letter;
      try {
        static_cast<void>(runtime_.unmount(drive_letter));
        container.mounted = false;
        container.drive_letter = wchar_t{};
        record_operation(OperationKind::container_unmounted,
                         OperationOutcome::succeeded,
                         container.name,
                         drive_letter);
      } catch (...) {
        record_operation(OperationKind::container_unmounted,
                         OperationOutcome::failed,
                         container.name,
                         drive_letter);
        throw;
      }
    }
  }
}

bool CardLoginMountSessionCore::force_unmount_active() {
  // Forced release is the card-removal path, so every retained secret goes
  // with it. A deliberate unmount keeps them, which is what lets the user
  // mount the container again in the same session.
  state_.hidden_container_password = security::SecureBuffer();
  state_.login_password = security::SecureBuffer();
  clear_operation_history();
  bool forced = false;
  for (const bool release_first : {true, false}) {
    for (SessionContainer& container : state_.containers) {
      if (!container.mounted || container.release_first != release_first) {
        continue;
      }
      forced = runtime_.unmount_forced(container.drive_letter).forced ||
               forced;
      container.mounted = false;
      container.drive_letter = wchar_t{};
    }
  }
  return forced;
}

bool CardLoginMountSessionCore::mounted() const noexcept {
  return state_.mounted();
}

}  // namespace detail
}  // namespace usb_vault::application
