#include "usb_vault/application/card_login_mount_session.h"

#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/container_index_store_v1.h"
#include "usb_vault/application/container_index_view_v1.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/application/product_settings_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_entry_path.h"
#include "usb_vault/volume/volume_file_operations.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "volume/drive_letter_pool.h"
#include "volume/mount_request.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::application {
namespace {

class ProductionCardLoginMountSessionRuntime final
    : public detail::CardLoginMountSessionRuntime {
 public:
  explicit ProductionCardLoginMountSessionRuntime(
      const volume::VolumeProvider& provider,
      std::filesystem::path volume_creator_helper_path)
      : provider_(provider),
        creator_(volume_creator_helper_path),
        imports_(std::move(volume_creator_helper_path)) {}

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

  [[nodiscard]] std::optional<volume::VolumeType> mounted_volume_type(
      const wchar_t drive_letter) override {
    for (const volume::MountedVolume& mounted : provider_.mounted_volumes()) {
      if (mounted.drive_letter == drive_letter) return mounted.type;
    }
    return std::nullopt;
  }

  [[nodiscard]] volume::VolumeSpace volume_space(
      const wchar_t drive_letter) override {
    return volume::volume_space(drive_letter);
  }

  [[nodiscard]] volume::MountIdentity mount_identity(wchar_t drive) override {
    return provider_.mount_identity(drive);
  }

  [[nodiscard]] std::uint64_t measure_protected_space(
      std::wstring_view path, const volume::MountIdentity& identity,
      const security::SecureBuffer& outer,
      const security::SecureBuffer& hidden) override {
    return imports_.protected_space(path, identity.drive_letter, identity.unique_id,
                                         identity.disk_length, outer, hidden);
  }

  void end_measurements() noexcept override { imports_.close(); }
  void end_imports() noexcept override { imports_.close(); }
  void import_protected(std::wstring_view container, const volume::MountIdentity& identity,
      const security::SecureBuffer& outer, const security::SecureBuffer& hidden,
      std::wstring_view source, std::wstring_view destination,
      const volume::TransferCancellation& cancelled, const std::function<void(unsigned int)>& progress) override {
    imports_.copy(container, identity.drive_letter, identity.unique_id, identity.disk_length,
                  outer, hidden, source, destination, [&](const auto& value) { if (progress) progress(value.percent); }, cancelled);
  }

  [[nodiscard]] security::SecureBuffer derive_container_password(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& root_secret,
      const security::KdfV1Salt& salt) override {
    return security::derive_volume_password_v1(password_input, root_secret,
                                                salt);
  }

  void format_partition(const volume::PartitionCandidate& target,
      const security::SecureBuffer& volume_password,
      const std::function<bool()>& confirm_erase,
      const CreationCancellation& cancelled) override {
    creator_.format_partition(target, volume_password, confirm_erase,
        [this](const volume_creation::CreationProgress& progress) {
          report_creation(progress, 0, 0);
        }, cancelled);
  }

  void format_partition_pair(const volume::PartitionCandidate& target, std::uint64_t hidden,
      const security::SecureBuffer& outer, const security::SecureBuffer& inner,
      const std::function<bool()>& confirm, const CreationCancellation& cancelled) override {
    creator_.format_partition_pair(target, hidden, outer, inner, confirm,
        [this](const auto& progress) { report_creation(progress, 0, 0); }, cancelled);
  }

  bool convert_partition(const volume::PartitionCandidate& target, bool begin, const security::SecureBuffer& password,
      const std::function<bool()>& confirm, const CreationCancellation& revoked, const CreationCancellation& pause) override {
    return creator_.convert_partition(target,begin,password,confirm,
        [this](const auto& progress){report_creation(progress,0,0);},revoked,pause);
  }

  volume::MountResult mount_partition(const volume::PartitionIdentity& identity,
      const security::SecureBuffer& password, const volume::MountOptions& options) override {
    return provider_.mount_partition(identity, password, options);
  }

  void create_standard(
      const std::wstring_view target_path, const std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) override {
    creator_.create_standard(target_path, size_bytes, volume_password,
        [this](const volume_creation::CreationProgress& progress) {
          report_creation(progress, 0, 0);
        },
        creation_cancelled);
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) override {
    creator_.create_with_hidden(target_path, outer_size_bytes,
                                hidden_size_bytes, outer_password,
                                hidden_password,
        [this, outer_size_bytes, hidden_size_bytes](const volume_creation::CreationProgress& progress) {
          report_creation(progress, outer_size_bytes, hidden_size_bytes);
        },
        creation_cancelled);
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
    DWORD drives = ::GetLogicalDrives();
    if (drives == 0) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "inspect available drive letters");
    }
    // A driver mount the DOS namespace has not published is still a letter in
    // use. Asking for one makes the mount manager land the volume elsewhere,
    // which the caller would otherwise never learn. The helper's own temporary
    // allocation reads both lists for the same reason.
    for (const volume::MountedVolume& mounted : provider_.mounted_volumes()) {
      const auto index = static_cast<unsigned>(mounted.drive_letter - L'A');
      if (index < 26) drives |= 1UL << index;
    }
    const wchar_t letter =
        volume::choose_free_drive_letter(static_cast<std::uint32_t>(drives));
    if (letter) return letter;
    throw std::system_error(
        std::make_error_code(std::errc::no_space_on_device),
        "no free drive letter is available for a container mount");
  }

  void notify_mounted(const wchar_t drive_letter) override {
    provider_.notify_mounted(drive_letter);
  }

  [[nodiscard]] std::vector<std::byte> read_container_index() override {
    const std::wstring path = container_index_v1_default_path();
    static_cast<void>(ensure_container_index_v1_file(path));
    return read_container_index_v1_file(path);
  }

  void write_container_index(
      const std::span<const std::byte> contents) override {
    write_container_index_v1_file(container_index_v1_default_path(), contents);
  }

  [[nodiscard]] bool remember_container_paths() override {
    return remember_container_paths_v1(product_settings_v1_default_path());
  }

  void set_remember_container_paths(const bool remember) override {
    set_remember_container_paths_v1(product_settings_v1_default_path(),
                                    remember);
  }

  [[nodiscard]] bool mount_imported_containers() override {
    return mount_imported_containers_v1(product_settings_v1_default_path());
  }
  bool force_unmount_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::force_unmount) != 0;
  }
  bool auto_mount_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::auto_mount) != 0;
  }
  void set_auto_mount_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::auto_mount, enabled ? 1U : 0U);
  }
  void set_force_unmount_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::force_unmount, enabled ? 1U : 0U);
  }
  bool operation_logging_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::operation_logging) != 0;
  }
  void set_operation_logging_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::operation_logging, enabled ? 1U : 0U);
  }

  void set_mount_imported_containers(const bool mount) override {
    set_mount_imported_containers_v1(product_settings_v1_default_path(),
                                     mount);
  }

  [[nodiscard]] volume::VolumeDirectoryListing list_directory(
      const std::wstring_view absolute_directory) override {
    return volume::list_volume_directory(absolute_directory);
  }

  void delete_entry(const std::wstring_view absolute_path) override {
    volume::delete_volume_entry(absolute_path);
  }

  void create_directory(const std::wstring_view absolute_path) override {
    volume::create_volume_directory(absolute_path);
  }

  void move_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination) override {
    volume::move_volume_entry(absolute_source, absolute_destination);
  }

  void move_entry_by_copying(
      const std::wstring_view absolute_source,
      const std::wstring_view absolute_destination) override {
    volume::move_volume_entry_by_copying(absolute_source, absolute_destination);
  }

  void copy_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination,
                  const volume::TransferCancellation& cancelled) override {
    volume::copy_volume_entry(absolute_source, absolute_destination, cancelled);
  }

 private:
  void report_creation(const volume_creation::CreationProgress& progress,
                       std::uint64_t outer, std::uint64_t inner) {
    if (!creation_feedback) return;
    using Stage = volume_creation::CreationStage;
    if (progress.stage == Stage::authorized) { creation_feedback(1, 100, 0); return; }
    if (progress.stage == Stage::allocated) { creation_feedback(2, 100, 0); return; }
    unsigned int percent = progress.percent;
    if (outer != 0 && inner != 0) {
      const auto total = outer + inner;
      if (progress.stage == Stage::formatting_outer) percent = static_cast<unsigned int>(outer * progress.percent / total);
      if (progress.stage == Stage::scanning_tail_space) percent = static_cast<unsigned int>(outer * 100 / total);
      if (progress.stage == Stage::formatting_hidden) percent = static_cast<unsigned int>((outer * 100 + inner * progress.percent) / total);
    }
    creation_feedback(3, percent, 0);
  }
  const volume::VolumeProvider& provider_;
  volume_creation::VolumeCreator creator_;
  volume_creation::VolumeImportSession imports_;
};

}  // namespace

ContainerCreationCleanupError::ContainerCreationCleanupError(
    std::exception_ptr operation_failure,
    std::exception_ptr cleanup_failure)
    : std::runtime_error(
          "container creation failed and incomplete-file cleanup failed"),
      operation_failure_(std::move(operation_failure)),
      cleanup_failure_(std::move(cleanup_failure)) {}

DecoyMaintenanceRefused::DecoyMaintenanceRefused()
    : std::runtime_error("this container cannot be opened for maintenance") {}

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

void CardLoginMountSession::set_creation_feedback(CreationFeedback feedback) {
  impl_->runtime.creation_feedback = std::move(feedback);
}

void CardLoginMountSession::set_creation_revocation(CreationCancellation revoked) {
  impl_->runtime.creation_revoked=std::move(revoked);
}
bool CardLoginMountSession::begin_partition_conversion(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return impl_->core.begin_partition_conversion(target,confirm);
}
bool CardLoginMountSession::resume_partition_conversion(std::uint32_t id) {
  return impl_->core.resume_partition_conversion(id);
}

void CardLoginMountSession::set_creation_cancellation(
    CreationCancellation cancelled) {
  impl_->runtime.creation_cancelled = std::move(cancelled);
}

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

std::uint32_t CardLoginMountSession::create_partition(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return impl_->core.create_partition(target, confirm);
}

std::uint32_t CardLoginMountSession::create_partition_pair(const volume::PartitionCandidate& target,
    std::uint64_t hidden, const security::SecureBuffer& decoy,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  if (!hidden || decoy.empty()) throw std::invalid_argument("partition pair inputs required");
  return impl_->core.create_partition(target, confirm, hidden, &decoy);
}

void CardLoginMountSession::format_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase) {
  impl_->core.format_partition(target, confirm_erase);
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

void CardLoginMountSession::complete_initialization(security::SecureBuffer password_utf8) {
  impl_->core.complete_initialization(std::move(password_utf8));
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

void CardLoginMountSession::mount_adopted_all() {
  impl_->core.mount_adopted_all();
}

wchar_t CardLoginMountSession::open_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  return impl_->core.open_decoy_maintenance(id, decoy_password_utf8);
}

bool CardLoginMountSession::verify_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  return impl_->core.verify_decoy_maintenance(id, decoy_password_utf8);
}

DecoyMaintenanceSurvey CardLoginMountSession::survey_decoy_maintenance(
    const security::SecureBuffer& decoy_password_utf8,
    const SurveyProgress& progress,
    const SurveyCancellation& cancelled) {
  return impl_->core.survey_decoy_maintenance(decoy_password_utf8, progress,
                                              cancelled);
}

DecoyMaintenanceSpace CardLoginMountSession::decoy_maintenance_space(
    const std::uint32_t id) {
  return impl_->core.decoy_maintenance_space(id);
}

bool CardLoginMountSession::decoy_maintenance_write_blocked(const std::uint32_t id) {
  return impl_->core.decoy_maintenance_write_blocked(id);
}

void CardLoginMountSession::end_decoy_maintenance(const std::uint32_t id) {
  impl_->core.end_decoy_maintenance(id);
}

void CardLoginMountSession::import_external_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view absolute_source,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress) {
  impl_->core.import_external_entry(id, relative_directory, absolute_source,
                                    cancelled, progress);
}

wchar_t CardLoginMountSession::open_surveyed_maintenance(
    const std::uint32_t id) {
  return impl_->core.open_surveyed_maintenance(id);
}

void CardLoginMountSession::unmount_container(const std::uint32_t id) {
  impl_->core.unmount_container(id);
}

void CardLoginMountSession::forget_container(const std::uint32_t id) {
  impl_->core.forget_container(id);
}

std::uint32_t CardLoginMountSession::adopt_container(
    const std::wstring_view path) {
  return impl_->core.adopt_container(path);
}

bool CardLoginMountSession::mounting_imported_containers() {
  return impl_->core.mounting_imported_containers();
}

void CardLoginMountSession::set_mounting_imported_containers(
    const bool mount) {
  impl_->core.set_mounting_imported_containers(mount);
}

bool CardLoginMountSession::remembering_container_paths() {
  return impl_->core.remembering_container_paths();
}

void CardLoginMountSession::set_remembering_container_paths(
    const bool remember) {
  impl_->core.set_remembering_container_paths(remember);
}

volume::VolumeDirectoryListing CardLoginMountSession::list_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory) {
  return impl_->core.list_container_directory(id, relative_directory);
}

void CardLoginMountSession::delete_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  impl_->core.delete_container_entry(id, relative_directory, name);
}

void CardLoginMountSession::rename_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name, const std::wstring_view new_name) {
  impl_->core.rename_container_entry(id, relative_directory, name, new_name);
}

void CardLoginMountSession::create_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  impl_->core.create_container_directory(id, relative_directory, name);
}

void CardLoginMountSession::move_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  impl_->core.move_container_entry(source_id, source_relative_directory, name,
                                   destination_id,
                                   destination_relative_directory);
}

void CardLoginMountSession::copy_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  impl_->core.copy_container_entry(source_id, source_relative_directory, name,
                                   destination_id,
                                   destination_relative_directory);
}

void CardLoginMountSession::export_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory) {
  impl_->core.export_container_entry(id, relative_directory, name,
                                     absolute_destination_directory);
}

void CardLoginMountSession::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  impl_->core.export_container_file(id, absolute_destination_directory);
}

std::vector<ContainerStatus> CardLoginMountSession::container_statuses() {
  return impl_->core.container_statuses();
}

std::vector<ContainerOperationTarget> CardLoginMountSession::container_operation_targets() {
  return impl_->core.container_operation_targets();
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

bool CardLoginMountSession::operation_logging_enabled() {
  return impl_->core.operation_logging_enabled();
}
bool CardLoginMountSession::force_unmount_enabled() { return impl_->core.force_unmount_enabled(); }
bool CardLoginMountSession::auto_mount_enabled() { return impl_->core.auto_mount_enabled(); }
void CardLoginMountSession::set_auto_mount_enabled(bool enabled) { impl_->core.set_auto_mount_enabled(enabled); }
void CardLoginMountSession::set_force_unmount_enabled(bool enabled) { impl_->core.set_force_unmount_enabled(enabled); }
void CardLoginMountSession::reorder_containers(std::span<const std::uint32_t> ids) { impl_->core.reorder_containers(ids); }

void CardLoginMountSession::set_operation_logging_enabled(const bool enabled) {
  impl_->core.set_operation_logging_enabled(enabled);
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

void CardLoginMountSession::force_unmount_container(const std::uint32_t id) {
  impl_->core.force_unmount_container(id);
}

void CardLoginMountSession::force_unmount_user_active() {
  impl_->core.force_unmount_user_active();
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

// A cancel that lands while the helper is finishing still means no container.
// Everything written so far is under the staging name, so throwing here hands
// the file to the same cleanup a failure uses: asking to cancel always ends
// with nothing published, whichever side won the race.
void throw_if_creation_cancelled(
    const CardLoginMountSessionRuntime& runtime) {
  if (runtime.creation_cancelled && runtime.creation_cancelled()) {
    throw std::system_error(
        volume_creation::make_error_code(
            volume_creation::CreationError::operation_cancelled),
        "container creation was cancelled");
  }
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

// Drive letters are compared with this and nothing here wants a locale
// involved in that. The volume layer has its own copy behind a private header;
// duplicating four lines is cheaper than reaching across that boundary.
[[nodiscard]] wchar_t upper_drive_letter(const wchar_t letter) noexcept {
  return (letter >= L'a' && letter <= L'z')
             ? static_cast<wchar_t>(letter - L'a' + L'A')
             : letter;
}

[[nodiscard]] SessionContainer* find_by_id(
    CardLoginMountSessionState& state, const std::uint32_t id) noexcept {
  for (SessionContainer& container : state.containers) {
    if (container.id == id) return &container;
  }
  return nullptr;
}

// The letter a mount asked for is a request; the letter the driver reports is
// where the volume is. Windows can land it elsewhere, and a session that
// writes down its own request then addresses a drive holding nothing -- every
// later listing, read and write goes to the wrong place, and the page blames
// the container. A mount the driver cannot account for is refused rather than
// recorded under a letter that was never confirmed.
[[nodiscard]] wchar_t settled_drive_letter(const volume::MountResult& mounted) {
  if (mounted.drive_letter < L'A' || mounted.drive_letter > L'Z') {
    throw std::runtime_error(
        "the driver did not report which drive the volume mounted on");
  }
  return mounted.drive_letter;
}

[[nodiscard]] SessionContainer* find_by_path(
    CardLoginMountSessionState& state,
    const std::wstring_view path) noexcept {
  for (SessionContainer& container : state.containers) {
    if (!container.partition && container.path == path) return &container;
  }
  return nullptr;
}

// Adoption is idempotent so a login mount, a residual-mount record and an
// explicit mount all converge on one entry per container path.
SessionContainer& adopt(CardLoginMountSessionState& state,
                        const std::wstring_view path,
                        const wchar_t drive_letter, const bool mounted,
                        const bool release_first = false,
                        const bool derivation_known = true,
                        const bool newest = false) {
  if (SessionContainer* const existing = find_by_path(state, path)) {
    existing->drive_letter = drive_letter;
    existing->mounted = mounted;
    existing->release_first = existing->release_first || release_first;
    existing->derivation_known =
        existing->derivation_known || derivation_known;
    return *existing;
  }
  state.containers.push_back({.id = state.next_container_id,
                              .path = std::wstring(path),
                              .name = file_name_of(path),
                              .drive_letter = drive_letter,
                              .mounted = mounted,
                              .release_first = release_first,
                              .derivation_known = derivation_known});
  ++state.next_container_id;
  if (newest) {
    std::rotate(state.containers.begin(), state.containers.end() - 1, state.containers.end());
    return state.containers.front();
  }
  return state.containers.back();
}

// The number a partition record carries, or zero when the record is not
// there yet. What a disk container is called comes from here rather than from
// anything the session counts, so the two can never disagree.
[[nodiscard]] std::uint32_t recorded_sequence(
    const std::span<const std::byte> key, const std::span<const std::byte> file,
    const volume::PartitionIdentity& identity) {
  for (const auto& entry : list_container_index_partition_entries(key, file))
    if (entry.partition == identity) return entry.sequence;
  return 0;
}

SessionContainer& adopt_partition(CardLoginMountSessionState& state,
    const volume::PartitionIdentity& identity, const std::uint32_t sequence) {
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid partition identity");
  for (auto& entry : state.containers) if (entry.partition == identity) return entry;
  if (state.next_container_id == UINT32_MAX) throw std::overflow_error("container IDs exhausted");
  SessionContainer entry;
  entry.id = state.next_container_id++;
  // The recorded creation sequence, so the name is the container's own and
  // does not move when the list is rearranged. A record with none left to read
  // falls back to a session-local count, which is what every record had
  // before the sequence was written down.
  const auto shown = sequence ? sequence : state.next_partition_ordinal;
  entry.name = L"磁盘容器 " + std::to_wstring(shown);
  // The fallback stays one past the highest number handed out, so a record
  // with no sequence never lands on one a recorded container answers to.
  state.next_partition_ordinal = std::max(state.next_partition_ordinal, shown + 1);
  entry.derivation_known = false;
  entry.partition = identity;
  state.containers.push_back(std::move(entry));
  // Newest first, the way a newly created file container is placed. The
  // index records the same position, so it survives the session.
  std::rotate(state.containers.begin(), state.containers.end() - 1,
              state.containers.end());
  return state.containers.front();
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
      login_mount_policy_(login_mount_policy) {
  state_.operation_logging_enabled = runtime_.operation_logging_enabled();
}

void CardLoginMountSessionCore::record_operation(
    const OperationKind kind,
    const OperationOutcome outcome,
    const std::wstring_view container_name,
    const wchar_t drive_letter) noexcept {
  if (!state_.operation_logging_enabled) return;
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
    adopt_remembered_containers();
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

security::SecureBuffer CardLoginMountSessionCore::container_index_key() {
  return derive_shared_container_index_key_v1(
      authentication_.root_secret_.bytes());
}

void CardLoginMountSessionCore::remember_container_path(
    const std::wstring_view path) noexcept {
  // Every failure here is swallowed on purpose. The container exists and is
  // mounted; refusing the whole operation because its path could not be
  // written down would destroy something real to protect a convenience. The
  // cost of the silence is that the path is simply not remembered, which the
  // session shows anyway for as long as it lasts.
  try {
    if (!runtime_.remember_container_paths()) return;
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    const ContainerIndexWriteV1 written =
        prepend_container_index_path_v1(key.bytes(), file, path);
    key.clear();
    if (written == ContainerIndexWriteV1::added) {
      runtime_.write_container_index(file);
    }
  } catch (...) {
  }
}

void CardLoginMountSessionCore::forget_container_path(
    const std::wstring_view path) noexcept {
  try {
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    const bool removed =
        remove_container_index_path_v1(key.bytes(), file, path);
    key.clear();
    if (removed) {
      runtime_.write_container_index(file);
    }
  } catch (...) {
  }
}

void CardLoginMountSessionCore::reorder_containers(
    const std::span<const std::uint32_t> ids) {
  if (!authentication_.authenticated()) throw std::logic_error("ordering requires authentication");
  if (ids.empty() || ids.size() != state_.containers.size() || ids.size() > 1024)
    throw std::invalid_argument("ordering must contain every current container");
  std::vector<SessionContainer> ordered;
  std::vector<std::wstring> paths;
  // The user drags one list holding both kinds, so the saved order has to name
  // both. Recording only the paths left every disk container with no position
  // of its own, to be re-derived from its random slot on the next login.
  std::vector<ContainerIndexRef> refs;
  ordered.reserve(ids.size());
  paths.reserve(ids.size());
  refs.reserve(ids.size());
  for (const auto id : ids) {
    const auto* entry = find_by_id(state_, id);
    if (!entry || std::any_of(ordered.begin(), ordered.end(),
        [id](const auto& item) { return item.id == id; }))
      throw std::invalid_argument("invalid container order");
    ordered.push_back(*entry);
    if (!entry->partition) paths.push_back(entry->path);
    refs.push_back(entry->partition
        ? ContainerIndexRef{.partition = *entry->partition}
        : ContainerIndexRef{.path = entry->path});
  }
  if (runtime_.remember_container_paths()) {
    auto key = container_index_key();
    auto file = runtime_.read_container_index();
    const auto remembered_paths = list_container_index_paths_v1(key.bytes(), file);
    for (const auto& path : paths) {
      if (std::any_of(remembered_paths.begin(), remembered_paths.end(), [&](const auto& saved) {
            return ::CompareStringOrdinal(path.c_str(), -1, saved.c_str(), -1, TRUE) == CSTR_EQUAL;
          })) continue;
      if (add_container_index_path_v1(key.bytes(), file, path) == ContainerIndexWriteV1::full)
        throw std::runtime_error("container index is full");
    }
    // A partition is recorded when it is created and its record goes when it is
    // forgotten, so the index knows every one the session holds. Ordering names
    // only what it can find rather than refusing the whole arrangement over a
    // record that is somehow not there.
    const auto recorded = list_container_index_partitions(key.bytes(), file);
    std::erase_if(refs, [&](const auto& ref) {
      return ref.partition && std::ranges::find(recorded, *ref.partition) == recorded.end();
    });
    reorder_container_index_v1(key.bytes(), file, refs);
    key.clear();
    runtime_.write_container_index(file);
  }
  state_.containers.swap(ordered);
}

bool CardLoginMountSessionCore::remembering_container_paths() {
  return runtime_.remember_container_paths();
}

void CardLoginMountSessionCore::set_remembering_container_paths(
    const bool remember) {
  // Turning it off stops new paths being written and stops the list coming
  // back; it does not go through the index deleting what is already there. A
  // switch that quietly destroyed records could not be undone by flipping it
  // again, and the user asked to stop remembering, not to forget.
  runtime_.set_remember_container_paths(remember);
}

void CardLoginMountSessionCore::adopt_remembered_containers() noexcept {
  try {
    if (!runtime_.remember_container_paths()) return;
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    // Records written before the creation sequence existed get one now, once.
    // A disk container's name depends on it, so settling it here is what stops
    // the name being recomputed -- and moving -- on every login.
    if (number_container_index_partitions_v1(key.bytes(), file))
      runtime_.write_container_index(file);
    const auto refs = list_container_index_refs_v1(key.bytes(), file);
    const auto partitions = list_container_index_partition_entries(key.bytes(), file);
    key.clear();
    // Oldest first, so the number a disk container shows counts the order they
    // were created in rather than where they sit. Each is adopted at the front,
    // which leaves them newest first -- the order the index holds and the list
    // shows.
    for (auto item = partitions.rbegin(); item != partitions.rend(); ++item)
      adopt_partition(state_, *item->partition, item->sequence).conversion_state =
          static_cast<std::uint32_t>(item->conversion);
    for (const auto& ref : refs) {
      if (ref.partition) continue;
      // A remembered path whose file is not here is passed over and left in
      // the index. A container on a drive that is not plugged in is absent in
      // exactly the way a deleted one is, and clearing a slot on that evidence
      // would throw the record away for good.
      if (!runtime_.container_exists(ref.path)) continue;
      static_cast<void>(adopt(state_, ref.path, wchar_t{}, false, false, false));
    }
    // Layout containers may already be adopted. Match all visible entries to
    // the saved order without changing mount flags or dropping absent paths.
    // Both kinds are ranked against one list now, so a disk container keeps the
    // position it was given instead of following the slot it landed in.
    const auto rank = [&](const auto& entry) {
      const auto found = std::find_if(refs.begin(), refs.end(), [&](const auto& ref) {
        if (entry.partition) return ref.partition && *ref.partition == *entry.partition;
        return !ref.partition && ::CompareStringOrdinal(
            ref.path.c_str(), -1, entry.path.c_str(), -1, TRUE) == CSTR_EQUAL;
      });
      return found - refs.begin();
    };
    std::stable_sort(state_.containers.begin(), state_.containers.end(),
        [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
  } catch (...) {
  }
}

CardLoginMountSessionCore::ProbedMount
CardLoginMountSessionCore::mount_with_either_derivation(
    const std::wstring_view path) {
  security::SecureBuffer standard_password;
  wchar_t drive_letter{};
  volume::MountOptions options{};
  try {
    standard_password = derive_standard_password();
    drive_letter = reserve_drive_letter();
    options = {.drive_letter = drive_letter,
               .prf = volume::Pkcs5Prf::sha512,
               .pim = 0,
               .read_only = false,
               .notify_shell = false};
    const volume::MountResult mounted =
        runtime_.mount_created(path, standard_password, options);
    standard_password.clear();
    return {.drive_letter = settled_drive_letter(mounted),
            .hidden_volume = false};
  } catch (const std::system_error& error) {
    standard_password.clear();
    // The user selects a file, never a container class, and the page must not
    // offer that choice either. A file the standard derivation cannot open is
    // either built from the login input or not a container of ours at all, and
    // the two stay indistinguishable until the second derivation has been
    // tried.
    if (error.code() != volume::make_error_code(
                            volume::DriverError::authentication_failed) ||
        state_.login_password.empty()) {
      throw;
    }
  } catch (...) {
    standard_password.clear();
    throw;
  }
  return {.drive_letter = settled_drive_letter(
              runtime_.mount_created(path, hidden_container_password(), options)),
          .hidden_volume = true};
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

bool CardLoginMountSessionCore::begin_partition_conversion(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return convert_partition(target,std::nullopt,confirm);
}
bool CardLoginMountSessionCore::resume_partition_conversion(std::uint32_t id) {
  if (!authentication_.authenticated()) throw std::logic_error("resume requires authentication");
  const auto entry=find_by_id(state_,id);
  if (!entry || !entry->partition || entry->mounted || entry->conversion_state!=2)
    throw std::logic_error("only a normally paused partition can resume");
  const auto target=runtime_.resolve_conversion_target(*entry->partition);
  return convert_partition(target,id,[](const auto&){return true;});
}
bool CardLoginMountSessionCore::convert_partition(const volume::PartitionCandidate& target,
    std::optional<std::uint32_t> resume_id, const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  if (!authentication_.authenticated() || partition_format_active_ || !runtime_.remember_container_paths() ||
      !confirm || !runtime_.creation_cancelled || !runtime_.creation_revoked)
    throw std::logic_error("conversion requires an idle authenticated session and lifecycle controls");
  const auto snapshot=target;
  const volume::PartitionIdentity identity{target.device_id,target.device_id_flags,
      static_cast<std::uint32_t>(target.layout),target.logical_sector_bytes,target.offset_bytes,target.length_bytes,target.partition_id};
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid conversion identity");
  if (resume_id) {
    const auto entry=find_by_id(state_,*resume_id);
    if (!entry || entry->partition!=identity || entry->mounted || entry->conversion_state!=2)
      throw std::logic_error("resume identity changed");
  } else {
    for (const auto& entry:state_.containers) if(entry.partition==identity) throw std::logic_error("partition already recorded");
  }
  const auto generation=authentication_.generation_;
  const auto pause=runtime_.creation_cancelled;
  const auto revoked_query=runtime_.creation_revoked;
  const auto revoked=[&]{return !authentication_.authenticated() || authentication_.generation_!=generation || revoked_query();};
  const auto check=[&]{if(revoked())throw std::system_error(volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled));};
  check();
  partition_format_active_=true;
  struct Reset {bool& value; ~Reset(){value=false;}} reset{partition_format_active_};
  auto password=derive_standard_password();
  if(password.size()!=security::kKdfV1OutputSize)throw std::logic_error("invalid conversion password");
  const auto id=resume_id ? *resume_id : adopt_partition(state_,identity,0).id;
  bool recorded=resume_id.has_value(), accepted=false, requested=false;
  auto persist=[&](PartitionConversionState status,bool insert){
    auto key=container_index_key(); auto file=runtime_.read_container_index();
    if(insert) {
      if(add_container_index_partition(key.bytes(),file,identity,status)!=ContainerIndexWriteV1::added)
        throw std::runtime_error("conversion record unavailable");
    } else {
      if(resume_id && status==PartitionConversionState::uncertain) {
        const auto entries=list_container_index_partition_entries(key.bytes(),file);
        const auto saved=std::ranges::find_if(entries,[&](const auto& entry){return entry.partition==identity;});
        if(saved==entries.end() || saved->conversion!=PartitionConversionState::paused)
          throw std::logic_error("durable resume permission changed");
      }
      if(!set_container_index_partition_state(key.bytes(),file,identity,status))
        throw std::runtime_error("conversion record missing");
    }
    const auto numbered=recorded_sequence(key.bytes(),file,identity);
    key.clear(); check(); runtime_.write_container_index(file);
    auto entry=find_by_id(state_,id);
    if(!entry)throw std::logic_error("conversion session changed");
    entry->conversion_state=static_cast<std::uint32_t>(status);
    // The record now carries a number; the row was named before it existed.
    if(numbered)entry->name=L"磁盘容器 "+std::to_wstring(numbered);
  };
  try {
    const bool paused=runtime_.convert_partition(snapshot,!resume_id,password,[&]{
      if(requested || revoked())return false; requested=true;
      if(!confirm(snapshot) || revoked())return false;
      // Consume the paused permission durably BEFORE any new disk write.
      persist(PartitionConversionState::uncertain,!resume_id); recorded=true; accepted=true;
      return !revoked();
    },revoked,pause);
    password.clear(); check();
    if(!accepted)throw std::logic_error("conversion returned without consent");
    persist(paused ? PartitionConversionState::paused : PartitionConversionState::normal,false);
    check();
    if(!paused && !pause()) {
      mount_adopted_container(id);
      if(revoked()) {unmount_container_impl(id,true); check();}
    }
    if(!paused && runtime_.creation_feedback)runtime_.creation_feedback(5,100,find_by_id(state_,id)->drive_letter);
    return paused;
  } catch (...) {
    if(!recorded)std::erase_if(state_.containers,[id](const auto& entry){return entry.id==id;});
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::create_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm,
    std::uint64_t hidden_bytes, const security::SecureBuffer* decoy) {
  if ((decoy == nullptr) != (hidden_bytes == 0) ||
      (decoy && (decoy->empty() || state_.login_password.empty() || hidden_bytes < 1024 * 1024 ||
       hidden_bytes % 65536 || hidden_bytes > target.length_bytes / 4 * 3)))
    throw std::invalid_argument("invalid partition pair inputs");
  if (!authentication_.authenticated()) throw std::logic_error("partition creation requires authentication");
  if (!runtime_.remember_container_paths()) throw std::logic_error("partition creation requires remembering containers");
  const volume::PartitionIdentity identity{target.device_id, target.device_id_flags,
      static_cast<std::uint32_t>(target.layout),
      target.logical_sector_bytes, target.offset_bytes, target.length_bytes, target.partition_id};
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid partition identity");
  for (const auto& entry : state_.containers)
    if (entry.partition == identity) throw std::logic_error("partition already recorded");
  auto key = container_index_key();
  auto index = runtime_.read_container_index();
  const auto insertion = prepend_container_index_partition(key.bytes(), index, identity);
  if (insertion != ContainerIndexWriteV1::added) throw std::runtime_error("partition index unavailable or already recorded");
  const auto sequence = recorded_sequence(key.bytes(), index, identity);
  key.clear();
  // Allocate the session row before irreversible work, but remove it on failure.
  const auto id = adopt_partition(state_, identity, sequence).id;
  bool recorded = false;
  try {
    format_partition(target, [&](const auto& frozen) {
      if (!confirm || !confirm(frozen)) return false;
      // Persist identity before any destructive write; failure refuses consent.
      // A later cancellation may leave an unmounted record, never a lost volume.
      runtime_.write_container_index(index);
      recorded = true;
      return true;
    }, hidden_bytes, decoy);
    if (decoy) {
      auto* entry = find_by_id(state_, id);
      entry->release_first = true;
      entry->derivation_known = true;
    }
    if (runtime_.creation_feedback) runtime_.creation_feedback(4, 0, 0);
    throw_if_creation_cancelled(runtime_);
    mount_adopted_container(id);
    try { throw_if_creation_cancelled(runtime_); }
    catch (...) {
      // Removal can arrive inside the driver mount. Track the mount first so
      // failed cleanup remains visible to the deferred card-removal retry.
      unmount_container_impl(id, true);
      throw;
    }
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, find_by_id(state_, id)->drive_letter);
    return id;
  } catch (...) {
    if (!recorded) std::erase_if(state_.containers, [id](const auto& entry) { return entry.id == id; });
    throw;
  }
}

void CardLoginMountSessionCore::format_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase,
    std::uint64_t hidden_bytes, const security::SecureBuffer* decoy) {
  if (!authentication_.authenticated() || partition_format_active_) {
    throw std::logic_error("partition format requires an idle authenticated session");
  }
  if (!confirm_erase) throw std::invalid_argument("partition erase confirmation required");
  const volume::PartitionCandidate snapshot = target;
  const auto confirmation = confirm_erase;
  const auto cancellation = runtime_.creation_cancelled;
  if (!cancellation) throw std::invalid_argument("partition cancellation query required");
  const auto generation = authentication_.generation_;
  partition_format_active_ = true;
  struct Reset final {
    bool& active;
    ~Reset() { active = false; }
  } reset{partition_format_active_};
  // This session is serialized. Physical-card events reach the worker through
  // the captured atomic cancellation query, not concurrent secret mutation.
  const auto cancelled = [&] {
    return !authentication_.authenticated() ||
        authentication_.generation_ != generation ||
        (cancellation && cancellation());
  };
  const auto check = [&] {
    if (cancelled()) throw std::system_error(volume_creation::make_error_code(
        volume_creation::CreationError::operation_cancelled),
        "partition format was cancelled");
  };
  check();
  auto password = decoy ? runtime_.derive_container_password(*decoy, authentication_.root_secret_, layout_.container_b.salt) : derive_standard_password();
  if (password.size() != security::kKdfV1OutputSize) {
    throw std::logic_error("partition password derivation returned an invalid size");
  }
  check();
  bool requested = false;
  bool accepted = false;
  const auto confirm = [&] {
    if (requested || cancelled()) return false;
    requested = true;
    try {
      accepted = confirmation(snapshot) && !cancelled();
    } catch (...) {
      accepted = false;
    }
    return accepted;
  };
  if (decoy) {
    const auto& inner = hidden_container_password();
    if (inner.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition inner key");
    runtime_.format_partition_pair(snapshot, hidden_bytes, password, inner, confirm, cancelled);
  } else runtime_.format_partition(snapshot, password, confirm, cancelled);
  password.clear();
  check();
  if (!accepted) throw std::logic_error("partition format completed without consent");
  // No file staging, removal, publication, index entry or mount is appropriate
  // here. A failure can leave partial ciphertext and must never trigger retry.
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
    throw_if_creation_cancelled(runtime_);
    if (runtime_.creation_feedback && runtime_.container_size_if_present(staging_path) == size_bytes) runtime_.creation_feedback(2, 100, 0);
    if (runtime_.creation_feedback) runtime_.creation_feedback(4, 0, 0);
    runtime_.publish_created(staging_path, path);
    published = true;
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);

    const wchar_t drive_letter = settled_drive_letter(
        runtime_.mount_created(path,
                               volume_password,
                               {.drive_letter = reserve_drive_letter(),
                                .prf = volume::Pkcs5Prf::sha512,
                                .pim = 0,
                                .read_only = false,
                                .notify_shell = false}));
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, drive_letter);
    const std::uint32_t id = adopt(state_, path, drive_letter, true, false, true, true).id;
    volume_password.clear();
    remember_container_path(path);
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
  // A zero-length hidden volume is refused here and not only in the UI layer
  // that already refuses it: this entry point is the application contract, and
  // every other size invariant of the pair is stated in this same condition.
  if (outer_size_bytes == 0 || hidden_size_bytes == 0 ||
      outer_size_bytes % 512ULL != 0 ||
      hidden_size_bytes % 512ULL != 0 ||
      hidden_size_bytes >= outer_size_bytes) {
    throw std::invalid_argument(
        "outer and hidden sizes must be non-zero, aligned, and leave room for "
        "the outer volume");
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
    throw_if_creation_cancelled(runtime_);
    if (runtime_.creation_feedback && runtime_.container_size_if_present(staging_path) == outer_size_bytes) runtime_.creation_feedback(2, 100, 0);
    if (runtime_.creation_feedback) runtime_.creation_feedback(4, 0, 0);
    runtime_.publish_created(staging_path, path);
    published = true;
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);

    const wchar_t drive_letter = settled_drive_letter(
        runtime_.mount_created(path,
                               hidden_password,
                               {.drive_letter = reserve_drive_letter(),
                                .prf = volume::Pkcs5Prf::sha512,
                                .pim = 0,
                                .read_only = false,
                                .notify_shell = false}));
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, drive_letter);
    const std::uint32_t id = adopt(state_, path, drive_letter, true, true, true, true).id;
    outer_password.clear();
    remember_container_path(path);
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

SessionContainer& CardLoginMountSessionCore::maintenance_target(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  if (decoy_password_utf8.empty()) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  return *entry;
}

wchar_t CardLoginMountSessionCore::mount_outer_for_maintenance(
    SessionContainer& entry,
    const security::SecureBuffer& outer_password,
    const MaintenanceMount mode) {
  // One file cannot serve two volumes, so whatever it serves now goes first.
  // Force is this flow's standing answer, agreed to before any of it starts.
  if (entry.mounted) unmount_container_impl(entry.id, true);

  // The hidden volume's password is the session's own, never the caller's: the
  // driver reads that header only to learn what to refuse writes to.
  const security::SecureBuffer& hidden_password = hidden_container_password();
  const auto refuse = [&]() {
    if (mode == MaintenanceMount::open) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       entry.name);
    }
    return DecoyMaintenanceRefused();
  };

  // Everything below -- the volume-type gate, the identity check and the
  // forced release when either refuses -- addresses this drive. It has to be
  // the one the driver reported, or a wrong-type mount would be left behind
  // while a letter that holds nothing is the one asked to let go of it.
  wchar_t drive_letter{};
  try {
    const volume::MountOptions options{.drive_letter = reserve_drive_letter(),
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false,
         .hidden_volume_protection =
             volume::HiddenVolumeProtection{.password = &hidden_password}};
    drive_letter = settled_drive_letter(entry.partition
        ? runtime_.mount_partition(*entry.partition, outer_password, options)
        : runtime_.mount_created(entry.path, outer_password, options));
  } catch (...) {
    // Every driver rejection means the same thing to everyone above.
    throw refuse();
  }

  // The driver reports the outer type only while protecting a hidden volume,
  // so anything else means the wrong volume came up -- the hidden one included.
  const std::optional<volume::VolumeType> type =
      runtime_.mounted_volume_type(drive_letter);
  if (!type.has_value() || *type != volume::VolumeType::outer) {
    try {
      static_cast<void>(runtime_.unmount_forced(drive_letter));
    } catch (...) {
      // Still this session's to release later, so it is recorded rather than
      // dropped. The derivation flags stay untouched: nothing was established.
      entry.mounted = true;
      entry.drive_letter = drive_letter;
      throw refuse();
    }
    throw refuse();
  }

  std::optional<volume::MountIdentity> identity;
  try {
    if (mode == MaintenanceMount::open) {
      identity = runtime_.mount_identity(drive_letter);
      if (!identity->protected_outer_writable || identity->drive_letter != drive_letter)
        throw std::runtime_error("maintenance mount identity unavailable");
    }
  } catch (...) {
    try { static_cast<void>(runtime_.unmount_forced(drive_letter)); }
    catch (...) { entry.mounted = true; entry.drive_letter = drive_letter; }
    throw refuse();
  }
  entry.maintenance_identity = std::move(identity);
  entry.maintenance_recovery_required = false;

  // Proven, not stored: this container has an outer volume the supplied
  // password opens and a hidden one the session's own derivation opens.
  entry.mounted = true;
  entry.drive_letter = drive_letter;
  entry.release_first = true;
  entry.derivation_known = true;
  entry.maintenance = mode == MaintenanceMount::open;
  if (mode == MaintenanceMount::open) {
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     entry.name,
                     drive_letter);
    runtime_.notify_mounted(drive_letter);
  }
  return drive_letter;
}

wchar_t CardLoginMountSessionCore::open_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  SessionContainer& entry = maintenance_target(id, decoy_password_utf8);
  if (entry.mounted && entry.maintenance) return entry.drive_letter;
  const bool switched = release_other_maintenance(id);
  security::SecureBuffer outer_password;
  try {
    outer_password = runtime_.derive_container_password(
        decoy_password_utf8, authentication_.root_secret_,
        layout_.container_b.salt);
    const wchar_t drive_letter = mount_outer_for_maintenance(
        entry, outer_password, MaintenanceMount::open);
    state_.decoy_outer_password = std::move(outer_password);
    return drive_letter;
  } catch (...) {
    outer_password.clear();
    if (switched) runtime_.end_measurements();
    throw;
  }
}

DecoyMaintenanceSurvey CardLoginMountSessionCore::survey_decoy_maintenance(
    const security::SecureBuffer& decoy_password_utf8,
    const SurveyProgress& progress,
    const SurveyCancellation& cancelled) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  if (decoy_password_utf8.empty()) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }

  // One derivation for the whole walk: every container of this kind shares the
  // salt, so paying per container would buy nothing.
  security::SecureBuffer outer_password = runtime_.derive_container_password(
      decoy_password_utf8, authentication_.root_secret_,
      layout_.container_b.salt);

  DecoyMaintenanceSurvey survey;
  const auto total = static_cast<std::uint32_t>(state_.containers.size());
  std::uint32_t done = 0;
  try {
    for (SessionContainer& entry : state_.containers) {
      if (cancelled && cancelled()) {
        survey.cancelled = true;
        break;
      }
      // A container known to open on the standard derivation has no outer
      // volume to find, so it is left exactly as the user is using it.
      if (!(entry.derivation_known && !entry.release_first)) {
        try {
          const wchar_t drive_letter = mount_outer_for_maintenance(
              entry, outer_password, MaintenanceMount::trial);
          static_cast<void>(runtime_.unmount_forced(drive_letter));
          entry.mounted = false;
          entry.maintenance = false;
          entry.maintenance_identity.reset();
          entry.drive_letter = wchar_t{};
          survey.maintainable.push_back(entry.id);
        } catch (const DecoyMaintenanceRefused&) {
          // Not one this password maintains, which is all anyone learns.
        } catch (...) {
          // A container that would not release, or a letter that could not be
          // taken: nothing was learned about it, and the walk goes on.
          ++survey.unreachable;
        }
      }
      ++done;
      if (progress) progress(done, total);
    }
  } catch (...) {
    outer_password.clear();
    throw;
  }

  // Keeping it is what spares the next page a second prompt; keeping it after
  // finding nothing would only be holding a secret for no one.
  if (!survey.maintainable.empty()) {
    state_.decoy_outer_password = std::move(outer_password);
  } else {
    outer_password.clear();
  }
  return survey;
}

DecoyMaintenanceSpace CardLoginMountSessionCore::decoy_maintenance_space(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  // Only a volume opened for maintenance is measured: measuring asks for
  // consent, and nothing else in the product has a reason to spend it.
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error("this container is not open for maintenance");
  }
  if (!entry->maintenance_identity || state_.decoy_outer_password.empty())
    throw std::logic_error("authenticated maintenance binding unavailable");
  const auto verify_identity = [&] {
    try {
      if (runtime_.mount_identity(entry->drive_letter) != *entry->maintenance_identity)
        throw std::runtime_error("maintenance mount identity changed");
    } catch (...) {
      runtime_.end_measurements();
      throw;
    }
  };
  if (entry->partition)
    throw std::logic_error("partition maintenance capacity measurement is not supported");
  verify_identity();
  const volume::VolumeSpace space = runtime_.volume_space(entry->drive_letter);
  const auto safe = runtime_.measure_protected_space(
      entry->path, *entry->maintenance_identity, state_.decoy_outer_password,
      hidden_container_password());
  verify_identity();
  return {.total_bytes = space.total_bytes,
          .free_bytes = space.free_bytes,
          .safe_free_bytes = safe};
}

bool CardLoginMountSessionCore::decoy_maintenance_write_blocked(const std::uint32_t id) {
  if (!authentication_.authenticated()) throw std::logic_error("maintenance unavailable");
  const SessionContainer* const entry = find_by_id(state_, id);
  if (!entry || !entry->mounted || !entry->maintenance)
    throw std::logic_error("maintenance unavailable");
  const auto type = runtime_.mounted_volume_type(entry->drive_letter);
  if (type == volume::VolumeType::outer_write_prevented) return true;
  if (type == volume::VolumeType::outer) return false;
  // Missing or unexpected volumes are unknown, never evidence of protection.
  throw std::runtime_error("maintenance status unavailable");
}

void CardLoginMountSessionCore::end_decoy_maintenance(const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error("this container is not open for maintenance");
  }
  // The deliberate unmount, not the forced one: ending maintenance is not an
  // emergency, and a volume still in use is the user's to settle.
  unmount_container_impl(id, false);
}

void CardLoginMountSessionCore::import_external_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view absolute_source,
    const volume::TransferCancellation& cancelled, const std::function<void(unsigned int)>& progress) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "external import requires an authenticated card session");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  // 🔴 Only a volume this session opened for maintenance. An ordinary mount
  // takes its files through the file manager, which the page already drives;
  // widening this entry would give a shell drag a way into any mounted volume
  // without the page having asked for one.
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error(
        "external import requires a volume open for maintenance");
  }
  // The privileged import protocol authenticates regular backing files only.
  // A partition must never be substituted as a file path or weaken that gate.
  if (entry->partition)
    throw std::logic_error("partition maintenance external import is not supported");
  // The only thing taken from the source path is the name it will have inside
  // the volume, and that name is then checked as any other page-supplied
  // component is. A root, a trailing separator or a `..` leaves nothing that
  // can be named, so the import is refused instead of guessing.
  const std::wstring name =
      std::filesystem::path(absolute_source).filename().wstring();
  if (name.empty()) {
    throw std::invalid_argument("the dropped entry has no file name");
  }
  const auto destination = volume::resolve_volume_child_path(entry->drive_letter, relative_directory, name).substr(3);
  if (!entry->maintenance_identity || state_.decoy_outer_password.empty() || entry->maintenance_recovery_required)
    throw std::logic_error("maintenance upload requires a fresh authenticated mount");
  try {
    const auto verify = [&] {
      if (!authentication_.authenticated() || runtime_.mount_identity(entry->drive_letter) != *entry->maintenance_identity)
        throw std::runtime_error("maintenance identity changed during upload");
    };
    verify();
    runtime_.import_protected(entry->path, *entry->maintenance_identity, state_.decoy_outer_password,
        hidden_container_password(), absolute_source, destination, cancelled, progress);
    verify();
  } catch (...) {
    entry->maintenance_recovery_required = true;
    runtime_.end_imports();
    throw;
  }
}

wchar_t CardLoginMountSessionCore::open_surveyed_maintenance(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.decoy_outer_password.empty()) {
    throw std::logic_error("this session has no surveyed decoy password");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (entry->mounted && entry->maintenance) {
    if (entry->maintenance_recovery_required) throw std::logic_error("end failed maintenance before reopening");
    return entry->drive_letter;
  }
  const bool switched = release_other_maintenance(id);
  try {
    return mount_outer_for_maintenance(*entry, state_.decoy_outer_password,
                                       MaintenanceMount::open);
  } catch (...) {
    if (switched) runtime_.end_measurements();
    throw;
  }
}

bool CardLoginMountSessionCore::release_other_maintenance(
    const std::uint32_t next_id) {
  bool released = false;
  for (const SessionContainer& container : state_.containers) {
    if (container.id == next_id || !container.maintenance) continue;
    unmount_container_impl(container.id, false, false);
    released = true;
  }
  return released;
}

bool CardLoginMountSessionCore::verify_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  SessionContainer& entry = maintenance_target(id, decoy_password_utf8);
  security::SecureBuffer outer_password = runtime_.derive_container_password(
      decoy_password_utf8, authentication_.root_secret_,
      layout_.container_b.salt);
  wchar_t drive_letter{};
  try {
    drive_letter = mount_outer_for_maintenance(entry, outer_password,
                                               MaintenanceMount::trial);
  } catch (const DecoyMaintenanceRefused&) {
    outer_password.clear();
    return false;
  } catch (...) {
    outer_password.clear();
    throw;
  }
  outer_password.clear();

  // Nothing stays up on the strength of having asked. A volume that will not
  // come down stays recorded as mounted and the failure reaches the caller.
  static_cast<void>(runtime_.unmount_forced(drive_letter));
  entry.mounted = false;
  entry.maintenance = false;
  entry.maintenance_identity.reset();
  entry.drive_letter = wchar_t{};
  return true;
}

void CardLoginMountSessionCore::complete_initialization(security::SecureBuffer password_utf8) {
  if (!authentication_.authenticated() || password_utf8.empty() ||
      state_.mounted() || !state_.containers.empty() || !state_.login_password.empty()) {
    throw std::logic_error("initialization handoff requires a fresh authenticated session");
  }
  // Move the exact normalized password confirmed before card initialization.
  // No second card authentication, key derivation, or automatic mount occurs.
  state_.login_password = std::move(password_utf8);
  clear_operation_history();
  record_operation(OperationKind::session_opened, OperationOutcome::succeeded);
}

security::SecureBuffer CardLoginMountSessionCore::copy_session_password() {
  if (!authentication_.authenticated()) return security::SecureBuffer();
  return duplicate_secret(state_.login_password);
}

void CardLoginMountSessionCore::release_session_secrets() noexcept {
  for (auto& container : state_.containers) container.maintenance_identity.reset();
  state_.hidden_container_password = security::SecureBuffer();
  state_.decoy_outer_password = security::SecureBuffer();
  state_.login_password = security::SecureBuffer();
  // The elevated runs belonged to the session that opened them.
  runtime_.end_imports();
  runtime_.end_measurements();
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
  if (entry->conversion_state) throw std::logic_error("incomplete conversion cannot mount");
  if (entry->partition) {
    const wchar_t drive = reserve_drive_letter();
    const volume::MountOptions options{.drive_letter = drive, .notify_shell = false};
    bool login_derived = entry->derivation_known && entry->release_first;
    volume::MountResult mounted{};
    if (!login_derived) {
      auto password = derive_standard_password();
      if (password.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition password");
      try { mounted = runtime_.mount_partition(*entry->partition, password, options); }
      catch (const std::system_error& error) {
        if (entry->derivation_known || state_.login_password.empty() ||
            error.code() != volume::make_error_code(volume::DriverError::authentication_failed)) throw;
        login_derived = true;
      }
    }
    if (login_derived) {
      const auto& password = hidden_container_password();
      if (password.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition password");
      mounted = runtime_.mount_partition(*entry->partition, password, options);
    }
    const wchar_t settled = settled_drive_letter(mounted);
    entry->drive_letter = settled;
    entry->mounted = true;
    entry->release_first = login_derived;
    entry->derivation_known = true;
    record_operation(OperationKind::container_mounted, OperationOutcome::succeeded, entry->name, settled);
    runtime_.notify_mounted(settled);
    return;
  }
  const std::wstring container_name = entry->name;
  security::SecureBuffer standard_password;
  bool mounted = false;
  try {
    if (!runtime_.container_exists(entry->path)) {
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "adopted container file is no longer present");
    }

    // A container read back from the index has never said which of its
    // volumes answers, so it is opened the way an import is: standard
    // derivation first, login derivation second. What answers is recorded, and
    // the next mount in this session no longer probes.
    wchar_t drive_letter{};
    bool hidden_volume = entry->release_first;
    if (!entry->derivation_known) {
      const ProbedMount probed = mount_with_either_derivation(entry->path);
      drive_letter = probed.drive_letter;
      hidden_volume = probed.hidden_volume;
    } else {
      if (hidden_volume && state_.login_password.empty()) {
        throw std::logic_error(
            "this container can only be opened by the login that captured it");
      }
      if (!hidden_volume) standard_password = derive_standard_password();
      const security::SecureBuffer& volume_password =
          hidden_volume ? hidden_container_password() : standard_password;
      drive_letter = settled_drive_letter(
          runtime_.mount_created(entry->path,
                                 volume_password,
                                 {.drive_letter = reserve_drive_letter(),
                                  .prf = volume::Pkcs5Prf::sha512,
                                  .pim = 0,
                                  .read_only = false,
                                  .notify_shell = false}));
    }
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    standard_password.clear();
    entry->drive_letter = drive_letter;
    entry->mounted = true;
    entry->release_first = hidden_volume;
    entry->derivation_known = true;
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

void CardLoginMountSessionCore::mount_adopted_all() {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting containers requires an authenticated card session");
  }
  // Q-07 fixes mounting at B-before-A and requires the run to abort on the
  // first failure, so this keeps the same {true, false} order unmount_active
  // uses rather than reversing it: a rollback must not leave A's drive letter
  // briefly visible. mount_adopted_container already returns early for a
  // container that is mounted, so a partially mounted session resumes.
  std::vector<std::uint32_t> pending;
  for (const bool release_first : {true, false}) {
    for (const SessionContainer& container : state_.containers) {
      if (container.mounted || container.conversion_state || container.release_first != release_first) {
        continue;
      }
      pending.push_back(container.id);
    }
  }
  // The order is settled before any mounting starts: mount_adopted_container
  // writes back into state_.containers, and reading the list while it does
  // would tie this loop to that function's internals.
  for (const std::uint32_t id : pending) mount_adopted_container(id);
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
  bool mounted = false;
  try {
    if (!runtime_.container_exists(path)) {
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "container file is unavailable");
    }
    const ProbedMount probed = mount_with_either_derivation(path);
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     probed.drive_letter);
    const std::uint32_t id = adopt(state_, path, probed.drive_letter, true,
                                   probed.hidden_volume, true, true)
                                 .id;
    remember_container_path(path);
    runtime_.notify_mounted(probed.drive_letter);
    return id;
  } catch (...) {
    if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::adopt_container(
    const std::wstring_view path) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "adopting a container requires an authenticated card session");
  }
  if (const SessionContainer* const adopted = find_by_path(state_, path);
      adopted != nullptr) {
    return adopted->id;
  }
  if (!runtime_.container_exists(path)) {
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "container file is unavailable");
  }
  // Nothing is recorded in the session log: no volume was touched, and the
  // row appearing in the list is the whole of what happened. This is the same
  // arrival a remembered path gets at login, and it says the same amount
  // about the container -- which is nothing, until something mounts it.
  const std::uint32_t id =
      adopt(state_, path, wchar_t{}, false, false, false, true).id;
  remember_container_path(path);
  return id;
}

bool CardLoginMountSessionCore::mounting_imported_containers() {
  return runtime_.mount_imported_containers();
}

void CardLoginMountSessionCore::set_mounting_imported_containers(
    const bool mount) {
  runtime_.set_mount_imported_containers(mount);
}

void CardLoginMountSessionCore::unmount_container(const std::uint32_t id) {
  unmount_container_impl(id, false);
}

void CardLoginMountSessionCore::force_unmount_container(const std::uint32_t id) {
  unmount_container_impl(id, true);
}

void CardLoginMountSessionCore::force_unmount_user_active() {
  for (const bool release_first : {true, false}) {
    for (const SessionContainer& container : state_.containers) {
      if (container.mounted && container.release_first == release_first)
        unmount_container_impl(container.id, true);
    }
  }
}

void CardLoginMountSessionCore::unmount_container_impl(
    const std::uint32_t id, const bool allow_force,
    const bool end_measurements) {
  for (SessionContainer& container : state_.containers) {
    if (container.id != id) continue;
    if (!container.mounted) return;
    const wchar_t drive_letter = container.drive_letter;
    const bool ended_maintenance = container.maintenance;
    try {
      if (allow_force) static_cast<void>(runtime_.unmount_forced(drive_letter));
      else static_cast<void>(runtime_.unmount(drive_letter));
      container.mounted = false;
      container.maintenance = false;
      container.maintenance_identity.reset();
      container.drive_letter = wchar_t{};
      if (ended_maintenance) runtime_.end_imports();
      if (ended_maintenance && end_measurements) runtime_.end_measurements();
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

void CardLoginMountSessionCore::forget_container(const std::uint32_t id) {
  for (auto entry = state_.containers.begin();
       entry != state_.containers.end(); ++entry) {
    if (entry->id != id) continue;
    if (entry->conversion_state) throw std::logic_error("incomplete conversion record cannot be forgotten");
    if (entry->mounted) {
      throw std::logic_error(
          "a mounted container cannot be dropped from the session");
    }
    // The path is copied out before the entry goes, because forgetting it is
    // what the caller asked for and the entry is where it is written down.
    if (entry->partition) {
      auto key = container_index_key();
      auto index = runtime_.read_container_index();
      if (remove_container_index_partition(key.bytes(), index, *entry->partition)) runtime_.write_container_index(index);
      state_.containers.erase(entry);
      return;
    }
    const std::wstring path = entry->path;
    state_.containers.erase(entry);
    forget_container_path(path);
    return;
  }
  throw std::invalid_argument("no container is adopted under that identifier");
}

wchar_t CardLoginMountSessionCore::mounted_drive_letter(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "reaching into a container requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (!entry->mounted) {
    throw std::logic_error(
        "that container is not mounted, so it has no contents to reach");
  }
  return entry->drive_letter;
}

volume::VolumeDirectoryListing
CardLoginMountSessionCore::list_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  return runtime_.list_directory(
      volume::resolve_volume_entry_path(drive_letter, relative_directory));
}

void CardLoginMountSessionCore::delete_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  runtime_.delete_entry(volume::resolve_volume_child_path(
      drive_letter, relative_directory, name));
}

void CardLoginMountSessionCore::rename_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name, const std::wstring_view new_name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  // Both ends are resolved before either is used, so a bad new name refuses
  // the rename rather than half-performing it.
  const std::wstring source = volume::resolve_volume_child_path(
      drive_letter, relative_directory, name);
  const std::wstring destination = volume::resolve_volume_child_path(
      drive_letter, relative_directory, new_name);
  runtime_.move_entry(source, destination);
}

void CardLoginMountSessionCore::create_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  runtime_.create_directory(volume::resolve_volume_child_path(
      drive_letter, relative_directory, name));
}

void CardLoginMountSessionCore::move_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  const wchar_t source_drive = mounted_drive_letter(source_id);
  const wchar_t destination_drive = mounted_drive_letter(destination_id);
  const std::wstring source = volume::resolve_volume_child_path(
      source_drive, source_relative_directory, name);
  const std::wstring destination = volume::resolve_volume_child_path(
      destination_drive, destination_relative_directory, name);
  // One identifier is one volume, and this session never puts two containers
  // on one letter, so the identifiers settle which move this is.
  if (source_id == destination_id) {
    runtime_.move_entry(source, destination);
    return;
  }
  runtime_.move_entry_by_copying(source, destination);
}

void CardLoginMountSessionCore::copy_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  const wchar_t source_drive = mounted_drive_letter(source_id);
  const wchar_t destination_drive = mounted_drive_letter(destination_id);
  runtime_.copy_entry(
      volume::resolve_volume_child_path(source_drive,
                                        source_relative_directory, name),
      volume::resolve_volume_child_path(destination_drive,
                                        destination_relative_directory, name),
      {});
}

void CardLoginMountSessionCore::export_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  const std::wstring destination = volume::resolve_exported_entry_path(
      absolute_destination_directory, name);

  // 🔴 A location on a drive this session has a container mounted on is not an
  // export, it is a copy, and the file manager already does copies without any
  // of what this call implies. Refusing it here also keeps the one entry point
  // that may leave a volume from being the convenient way to do everything.
  for (const SessionContainer& container : state_.containers) {
    if (container.mounted &&
        upper_drive_letter(container.drive_letter) ==
            upper_drive_letter(destination[0])) {
      throw std::invalid_argument(
          "that location is inside a container, which is a copy and not an "
          "export");
    }
  }

  // 🔴 A location on a drive this session has a container mounted on is not an
  // export, it is a copy, and the file manager already does copies without any
  // of what this call implies. Refusing it here also keeps the one entry point
  // that may leave a volume from being the convenient way to do everything.
  // 🔴 No `record_operation` here, and none below it. The operation history is
  // a closed schema with no export in it, and Q-04 makes leaving it that way
  // this product's obligation.
  runtime_.copy_entry(
      volume::resolve_volume_child_path(drive_letter, relative_directory, name),
      destination, {});
}

void CardLoginMountSessionCore::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "migrating a container requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (entry->partition) throw std::logic_error("partition ciphertext export is unsupported");
  // 🔴 A mounted container is one the driver is writing to, so a copy taken
  // now is a copy of something in flight. Q-04 forbids handing back an
  // incomplete copy as a usable migration result, and the only way to be sure
  // of that here is to refuse rather than to copy and hope.
  if (entry->mounted) {
    throw std::logic_error(
        "unmount the container before migrating it, so what is copied is a "
        "volume nothing is writing to");
  }

  // Byte for byte, under the container's own name. Nothing is decrypted,
  // nothing is re-encrypted, nothing is added: what comes out is the file that
  // went in. No `record_operation` here either -- a migration is an export,
  // and Q-04 says an export leaves no trace of itself behind.
  runtime_.copy_entry(entry->path,
                      volume::resolve_exported_entry_path(
                          absolute_destination_directory, entry->name),
                      {});
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
                        .drive_letter = wchar_t{},
                        .partition = container.partition.has_value(), .conversion_state=container.conversion_state});
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
                      .drive_letter = container.drive_letter,
                      .partition = container.partition.has_value(), .conversion_state=container.conversion_state});
  }
  return result;
}

std::vector<ContainerOperationTarget> CardLoginMountSessionCore::container_operation_targets() {
  std::vector<ContainerOperationTarget> result;
  if (!authentication_.authenticated()) return result;
  for (const auto& entry : state_.containers)
    result.push_back({entry.id, entry.name, entry.mounted});
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
      const bool ended_maintenance = container.maintenance;
      try {
        static_cast<void>(runtime_.unmount(drive_letter));
        container.mounted = false;
        container.maintenance = false;
        container.maintenance_identity.reset();
        container.drive_letter = wchar_t{};
        if (ended_maintenance) { runtime_.end_imports(); runtime_.end_measurements(); }
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
  release_session_secrets();
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
      container.maintenance = false;
      container.maintenance_identity.reset();
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
