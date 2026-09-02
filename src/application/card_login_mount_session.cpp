#include "usb_vault/application/card_login_mount_session.h"

#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "volume/mount_request.h"

#include <Windows.h>

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
    const std::uint64_t outer_size_bytes,
    const std::uint64_t hidden_size_bytes,
    const security::SecureBuffer& decoy_password_utf8) {
  return impl_->core.create_container_b(outer_size_bytes, hidden_size_bytes,
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

void CardLoginMountSession::unmount_active() {
  impl_->core.unmount_active();
}

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
    state_.container_b_password = security::SecureBuffer();
    // Retained for the card-record rewrite alone. It is the password itself
    // rather than a derivative because rewrapping an envelope needs the
    // password as key material, and the record has no third slot to spare.
    state_.login_password = duplicate_secret(login_password_utf8);
    if (runtime_.container_exists(layout_.container_b.path)) {
      // Derived here because this is the only moment the login input exists.
      state_.container_b_password = runtime_.derive_container_password(
          login_password_utf8, authentication_.root_secret_,
          layout_.container_b.salt);
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

  security::SecureBuffer volume_password = derive_standard_password();
  bool staging_owned = false;
  bool published = false;
  try {
    runtime_.create_standard(staging_path, size_bytes, volume_password);
    staging_owned = true;
    runtime_.publish_created(staging_path, path);
    published = true;

    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(
        path, volume_password,
        {.drive_letter = drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false}));
    const std::uint32_t id = adopt(state_, path, drive_letter, true).id;
    volume_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    volume_password.clear();
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::create_container_b(
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

  const std::wstring_view path = layout_.container_b.path;
  if (find_by_path(state_, path) != nullptr) {
    throw std::logic_error("container path is already adopted");
  }
  const std::wstring staging_path = staging_path_for(path);
  if (runtime_.container_exists(path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container creation target already exists");
  }

  // The password the caller is logged in under opens the hidden volume; the
  // password set here opens the outer one. The session never decides which of
  // the two the caller "really" uses.
  security::SecureBuffer outer_password = runtime_.derive_container_password(
      decoy_password_utf8, authentication_.root_secret_,
      layout_.container_b.salt);
  security::SecureBuffer hidden_password = runtime_.derive_container_password(
      state_.login_password, authentication_.root_secret_,
      layout_.container_b.salt);
  bool staging_owned = false;
  bool published = false;
  try {
    runtime_.create_with_hidden(staging_path, outer_size_bytes,
                                hidden_size_bytes, outer_password,
                                hidden_password);
    staging_owned = true;
    runtime_.publish_created(staging_path, path);
    published = true;

    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(
        path, hidden_password,
        {.drive_letter = drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false}));
    const std::uint32_t id =
        adopt(state_, path, drive_letter, true, true).id;
    // The same value the login path derives when the container already
    // exists. Retaining it here is what lets this container be unmounted and
    // mounted again in the session that created it.
    state_.container_b_password = duplicate_secret(hidden_password);
    outer_password.clear();
    hidden_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    outer_password.clear();
    hidden_password.clear();
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
  if (!runtime_.container_exists(entry->path)) {
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "adopted container file is no longer present");
  }

  const bool hidden_volume = entry->release_first;
  if (hidden_volume && state_.container_b_password.empty()) {
    throw std::logic_error(
        "this container can only be opened by the login that captured it");
  }
  security::SecureBuffer standard_password;
  if (!hidden_volume) standard_password = derive_standard_password();
  const security::SecureBuffer& volume_password =
      hidden_volume ? state_.container_b_password : standard_password;

  const wchar_t drive_letter = reserve_drive_letter();
  try {
    static_cast<void>(runtime_.mount_created(
        entry->path, volume_password,
        {.drive_letter = drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false}));
  } catch (...) {
    standard_password.clear();
    throw;
  }
  standard_password.clear();
  entry->drive_letter = drive_letter;
  entry->mounted = true;
  runtime_.notify_mounted(drive_letter);
}

std::uint32_t CardLoginMountSessionCore::mount_container(
    const std::wstring_view path) {
  if (path == layout_.container_b.path) {
    // Its volumes derive from a password this session no longer holds, so the
    // fixed standard derivation must never be tried against it.
    throw std::invalid_argument(
        "this container cannot be opened with the standard derivation");
  }
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting a container requires an authenticated card session");
  }
  if (const SessionContainer* const adopted = find_by_path(state_, path);
      adopted != nullptr && adopted->mounted) {
    throw std::logic_error("container is already mounted");
  }
  if (!runtime_.container_exists(path)) {
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "container file is unavailable");
  }

  security::SecureBuffer volume_password = derive_standard_password();
  try {
    const wchar_t drive_letter = reserve_drive_letter();
    static_cast<void>(runtime_.mount_created(
        path, volume_password,
        {.drive_letter = drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false}));
    const std::uint32_t id = adopt(state_, path, drive_letter, true).id;
    volume_password.clear();
    runtime_.notify_mounted(drive_letter);
    return id;
  } catch (...) {
    volume_password.clear();
    throw;
  }
}

void CardLoginMountSessionCore::unmount_container(const std::uint32_t id) {
  for (SessionContainer& container : state_.containers) {
    if (container.id != id) continue;
    if (!container.mounted) return;
    static_cast<void>(runtime_.unmount(container.drive_letter));
    container.mounted = false;
    container.drive_letter = wchar_t{};
    return;
  }
  throw std::invalid_argument("no container is adopted under that identifier");
}

std::vector<ContainerStatus> CardLoginMountSessionCore::container_statuses() {
  std::vector<ContainerStatus> result;
  result.reserve(state_.containers.size());
  for (const SessionContainer& container : state_.containers) {
    const std::optional<std::uint64_t> size =
        runtime_.container_size_if_present(container.path);
    if (!size.has_value()) {
      if (container.mounted) {
        throw std::runtime_error(
            "mounted container backing file is unavailable");
      }
      result.push_back({.id = container.id,
                        .name = container.name,
                        .mounted = false,
                        .size_bytes = 0,
                        .drive_letter = wchar_t{}});
      continue;
    }
    result.push_back({.id = container.id,
                      .name = container.name,
                      .mounted = container.mounted,
                      .size_bytes = *size,
                      .drive_letter = container.mounted
                                          ? container.drive_letter
                                          : wchar_t{}});
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
      static_cast<void>(runtime_.unmount(container.drive_letter));
      container.mounted = false;
      container.drive_letter = wchar_t{};
    }
  }
}

bool CardLoginMountSessionCore::force_unmount_active() {
  // Forced release is the card-removal path, so every retained secret goes
  // with it. A deliberate unmount keeps them, which is what lets the user
  // mount the container again in the same session.
  state_.container_b_password = security::SecureBuffer();
  state_.login_password = security::SecureBuffer();
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
