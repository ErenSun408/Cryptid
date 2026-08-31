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
    volume::detail::validate_volume_path(path);
    const std::wstring owned_path(path);
    const DWORD attributes = ::GetFileAttributesW(owned_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
      }
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "inspect container creation path");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      throw std::invalid_argument(
          "container creation path refers to a directory");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::invalid_argument(
          "container creation path must not be a reparse point");
    }
    return true;
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
       std::filesystem::path volume_creator_helper_path)
      : runtime(provider, std::move(volume_creator_helper_path)),
        core(authentication, runtime, std::move(layout)) {}

  ProductionCardLoginMountSessionRuntime runtime;
  detail::CardLoginMountSessionCore core;
};

CardLoginMountSession::CardLoginMountSession(
    CardAuthenticationSession& authentication,
    const volume::VolumeProvider& provider,
    ContainerLayout layout,
    std::filesystem::path volume_creator_helper_path)
    : impl_(std::make_unique<Impl>(authentication, provider,
                                   std::move(layout),
                                   std::move(volume_creator_helper_path))) {}

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

[[nodiscard]] security::SecureBuffer container_a_password_input() {
  security::SecureBuffer input(kContainerAPasswordInput.size());
  std::ranges::copy(kContainerAPasswordInput, input.bytes().begin());
  return input;
}

[[nodiscard]] std::wstring container_a_staging_path(
    const ContainerLayout& layout) {
  std::wstring path(layout.container_a.path);
  path += L".preparing";
  return path;
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

void record_residual_mount(const ContainerLayout& layout,
                           CardLoginMountSessionState& state,
                           const wchar_t drive_letter) noexcept {
  if (drive_letter == layout.container_a.drive_letter) {
    state.container_a_mounted = true;
  }
  if (drive_letter == layout.container_b.drive_letter) {
    state.container_b_mounted = true;
  }
}

}  // namespace

CardLoginMountSessionCore::CardLoginMountSessionCore(
    CardAuthenticationSession& authentication,
    CardLoginMountSessionRuntime& runtime,
    ContainerLayout layout)
    : authentication_(authentication),
      runtime_(runtime),
      layout_(std::move(layout)) {}

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
    const MountSessionResult result = runtime_.mount_authenticated(
        layout_, login_password_utf8, authentication_.root_secret_);
    state_.container_a_mounted = result.container_a.has_value();
    state_.container_b_mounted = result.container_b.has_value();
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
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "container creation requires an authenticated card session");
  }
  if (state_.container_a_mounted) {
    throw std::logic_error("container A is already mounted");
  }
  if (size_bytes == 0 || size_bytes % 512ULL != 0) {
    throw std::invalid_argument(
        "container A size must be a non-zero multiple of 512 bytes");
  }

  const std::wstring staging_path = container_a_staging_path(layout_);
  if (runtime_.container_exists(layout_.container_a.path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container A creation target already exists");
  }

  security::SecureBuffer fixed_input = container_a_password_input();
  security::SecureBuffer volume_password;
  try {
    volume_password = runtime_.derive_container_password(
        fixed_input, authentication_.root_secret_, layout_.container_a.salt);
    fixed_input.clear();
  } catch (...) {
    fixed_input.clear();
    throw;
  }

  bool staging_owned = false;
  bool published = false;
  try {
    runtime_.create_standard(staging_path, size_bytes, volume_password);
    staging_owned = true;
    runtime_.publish_created(staging_path, layout_.container_a.path);
    published = true;

    static_cast<void>(runtime_.mount_created(
        layout_.container_a.path, volume_password,
        {.drive_letter = layout_.container_a.drive_letter,
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false}));
    state_.container_a_mounted = true;
    volume_password.clear();
    runtime_.notify_mounted(layout_.container_a.drive_letter);
  } catch (...) {
    volume_password.clear();
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

void CardLoginMountSessionCore::unmount_active() {
  if (state_.container_b_mounted) {
    static_cast<void>(runtime_.unmount(layout_.container_b.drive_letter));
    state_.container_b_mounted = false;
  }
  if (state_.container_a_mounted) {
    static_cast<void>(runtime_.unmount(layout_.container_a.drive_letter));
    state_.container_a_mounted = false;
  }
}

bool CardLoginMountSessionCore::force_unmount_active() {
  bool forced = false;
  if (state_.container_b_mounted) {
    forced = runtime_.unmount_forced(
        layout_.container_b.drive_letter).forced;
    state_.container_b_mounted = false;
  }
  if (state_.container_a_mounted) {
    forced = runtime_.unmount_forced(
                 layout_.container_a.drive_letter).forced ||
             forced;
    state_.container_a_mounted = false;
  }
  return forced;
}

bool CardLoginMountSessionCore::mounted() const noexcept {
  return state_.mounted();
}

}  // namespace detail
}  // namespace usb_vault::application
