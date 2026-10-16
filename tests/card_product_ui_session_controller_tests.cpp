#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/backup/export_package_file.h"
#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/security/offline_card_password_policy.h"
#include "usb_vault/volume/volume_space.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <algorithm>
#include <array>
#include <map>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::ActivationInstallResult;
using usb_vault::application::CardAuthenticationSession;
using usb_vault::licensing::ActivationState;
using usb_vault::application::DecoyMaintenanceSurvey;
using usb_vault::application::CardEntryContextController;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ContainerAStatus;
using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::ConfirmedPassword;
using usb_vault::security::OfflineCardPasswordPolicy;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;
using usb_vault::ui::CardProductUiState;
using usb_vault::ui::ContainerUiStatus;
using usb_vault::ui::DirectoryUiListing;
using usb_vault::ui::kContainerCreationMaximumMib;
using usb_vault::ui::kContainerCreationMinimumMib;
using usb_vault::ui::OperationUiKind;
using usb_vault::ui::OperationUiOutcome;

static_assert(std::derived_from<CardEntryContextController,
                                ICardEntryActions>);

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer root_secret() {
  SecureBuffer result(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

void require_wiped(const std::span<const std::byte> input,
                   const char* const message) {
  for (const std::byte value : input) {
    require(value == std::byte{0}, message);
  }
}

[[nodiscard]] bool equals(const std::span<const std::byte> left,
                          const std::span<const std::byte> right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

class FakeVolumeSession final : public IAuthenticatedCardVolumeSession {
 public:
  explicit FakeVolumeSession(CardAuthenticationSession& authentication)
      : authentication_(authentication) {}

  void create_container_a(const std::uint64_t size_bytes) override {
    ++create_a_calls;
    created_a_size_bytes = size_bytes;
    if (reject_create_a) throw std::runtime_error("creation failed");
    container_a_exists = true;
    container_a_size_bytes = size_bytes;
    container_a_mounted = true;
    mounted_state = true;
  }

  std::uint32_t create_container(const std::wstring_view path,
                                 const std::uint64_t size_bytes) override {
    ++create_calls;
    created_path.assign(path);
    created_size_bytes = size_bytes;
    if (reject_create) throw std::runtime_error("creation failed");
    return adopt(path, size_bytes);
  }

  std::uint32_t create_partition_pair(const usb_vault::volume::PartitionCandidate& target,
      std::uint64_t hidden, const SecureBuffer& decoy,
      const std::function<bool(const usb_vault::volume::PartitionCandidate&)>& confirm) override {
    require(confirm(target), "partition consent missing");
    ++create_b_calls; created_hidden_bytes=hidden;
    b_decoy_password.assign(decoy.bytes().begin(), decoy.bytes().end());
    return 1;
  }

  std::uint32_t create_container_b(
      const std::wstring_view path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer& decoy_password_utf8) override {
    ++create_b_calls;
    created_b_path.assign(path);
    created_outer_bytes = outer_size_bytes;
    created_hidden_bytes = hidden_size_bytes;
    b_decoy_password.assign(decoy_password_utf8.bytes().begin(),
                            decoy_password_utf8.bytes().end());
    if (reject_create_b) throw std::runtime_error("container B failed");
    return adopt(path, outer_size_bytes);
  }

  DecoyMaintenanceSurvey survey_decoy_maintenance(
      const SecureBuffer& decoy_password_utf8,
      const usb_vault::application::SurveyProgress& progress,
      const usb_vault::application::SurveyCancellation& cancelled) override {
    ++survey_calls;
    survey_password.assign(decoy_password_utf8.bytes().begin(),
                           decoy_password_utf8.bytes().end());
    if (reject_survey) throw std::runtime_error("survey failed");
    if (progress) progress(1, 1);
    if (cancelled && cancelled()) return {.cancelled = true};
    return {.maintainable = survey_result, .unreachable = survey_unreachable};
  }

  wchar_t open_surveyed_maintenance(const std::uint32_t id) override {
    ++open_maintenance_calls;
    opened_maintenance_id = id;
    if (reject_open_maintenance) throw std::runtime_error("refused");
    return L'Q';
  }

  usb_vault::application::DecoyMaintenanceSpace decoy_maintenance_space(
      const std::uint32_t id) override {
    ++maintenance_space_calls;
    maintenance_space_id = id;
    if (reject_maintenance_space) throw std::runtime_error("measure failed");
    return maintenance_space;
  }

  void end_decoy_maintenance(const std::uint32_t id) override {
    ++end_maintenance_calls;
    ended_maintenance_id = id;
  }

  std::uint32_t mount_container(const std::wstring_view path) override {
    ++mount_calls;
    mounted_path.assign(path);
    if (reject_mount) throw std::runtime_error("mount failed");
    for (Adopted& entry : containers) {
      if (entry.path != path) continue;
      if (entry.mounted) throw std::logic_error("container already mounted");
      entry.mounted = true;
      mounted_state = true;
      return entry.id;
    }
    return adopt(path, kFakeContainerBytes);
  }

  void mount_adopted_container(const std::uint32_t id) override {
    ++mount_adopted_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = true;
      mounted_state = true;
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  void unmount_container(const std::uint32_t id) override {
    ++unmount_container_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = false;
      mounted_state = any_mounted();
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  [[nodiscard]] bool remembering_container_paths() override {
    return remember_paths;
  }

  void set_remembering_container_paths(const bool remember) override {
    remember_paths = remember;
    ++remember_paths_writes;
  }

  std::uint32_t adopt_container(const std::wstring_view path) override {
    ++adopt_calls;
    adopted_path.assign(path);
    if (reject_adopt) throw std::runtime_error("adopt failed");
    for (const Adopted& entry : containers) {
      if (entry.path == path) return entry.id;
    }
    // An adopted container joins the list with no drive letter and no size:
    // nothing has opened it, so nothing is known about it yet.
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
                                       ? path
                                       : path.substr(separator + 1);
    const std::uint32_t id = next_id++;
    containers.push_back({.id = id,
                          .path = std::wstring(path),
                          .name = std::wstring(name),
                          .mounted = false});
    return id;
  }

  [[nodiscard]] bool mounting_imported_containers() override {
    return mount_imported;
  }

  void set_mounting_imported_containers(const bool mount) override {
    mount_imported = mount;
    ++mount_imported_writes;
  }

  bool remember_paths{true};
  int remember_paths_writes{};
  bool mount_imported{};
  int mount_imported_writes{};
  int adopt_calls{};
  bool reject_adopt{};
  std::wstring adopted_path;

  void forget_container(const std::uint32_t id) override {
    ++forget_container_calls;
    for (auto entry = containers.begin(); entry != containers.end(); ++entry) {
      if (entry->id != id) continue;
      if (entry->mounted) {
        throw std::logic_error(
            "a mounted container cannot be dropped from the session");
      }
      containers.erase(entry);
      mounted_state = any_mounted();
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  // The file manager, recorded rather than performed. What this layer is
  // answerable for is which container and which strings it passed on, and
  // whether it refused before passing anything on at all.
  struct DirectoryCall final {
    std::uint32_t id{};
    std::wstring directory;
  };

  struct EntryCall final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring name;
    std::wstring new_name;
  };

  struct TransferCall final {
    std::uint32_t source_id{};
    std::wstring source_directory;
    std::wstring name;
    std::uint32_t destination_id{};
    std::wstring destination_directory;
  };

  [[nodiscard]] usb_vault::volume::VolumeDirectoryListing
  list_container_directory(
      const std::uint32_t id,
      const std::wstring_view relative_directory) override {
    listed.push_back({.id = id, .directory = std::wstring(relative_directory)});
    if (reject_listing) throw std::invalid_argument("no such directory");
    return listing;
  }

  void delete_container_entry(const std::uint32_t id,
                              const std::wstring_view relative_directory,
                              const std::wstring_view name) override {
    deleted.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .name = std::wstring(name)});
  }

  void rename_container_entry(const std::uint32_t id,
                              const std::wstring_view relative_directory,
                              const std::wstring_view name,
                              const std::wstring_view new_name) override {
    renamed.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .name = std::wstring(name),
                       .new_name = std::wstring(new_name)});
  }

  void create_container_directory(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name) override {
    created_directories.push_back({.id = id,
                                   .directory =
                                       std::wstring(relative_directory),
                                   .name = std::wstring(name)});
  }

  void move_container_entry(
      const std::uint32_t source_id,
      const std::wstring_view source_relative_directory,
      const std::wstring_view name, const std::uint32_t destination_id,
      const std::wstring_view destination_relative_directory) override {
    moved.push_back(
        {.source_id = source_id,
         .source_directory = std::wstring(source_relative_directory),
         .name = std::wstring(name),
         .destination_id = destination_id,
         .destination_directory =
             std::wstring(destination_relative_directory)});
  }

  void copy_container_entry(
      const std::uint32_t source_id,
      const std::wstring_view source_relative_directory,
      const std::wstring_view name, const std::uint32_t destination_id,
      const std::wstring_view destination_relative_directory) override {
    copied.push_back(
        {.source_id = source_id,
         .source_directory = std::wstring(source_relative_directory),
         .name = std::wstring(name),
         .destination_id = destination_id,
         .destination_directory =
             std::wstring(destination_relative_directory)});
  }

  struct ImportCall final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring source;
    bool cancellation_asked{};
  };

  void import_external_entry(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view absolute_source,
      const std::function<bool()>& cancelled, const std::function<void(unsigned int)>&) override {
    imported.push_back({.id = id,
                        .directory = std::wstring(relative_directory),
                        .source = std::wstring(absolute_source),
                        .cancellation_asked = cancelled && cancelled()});
  }

  std::vector<ImportCall> imported;

  [[nodiscard]] std::size_t file_manager_calls() const noexcept {
    return listed.size() + deleted.size() + renamed.size() +
           created_directories.size() + moved.size() + copied.size() +
           imported.size();
  }

  usb_vault::volume::VolumeDirectoryListing listing;
  bool reject_listing{};
  std::vector<DirectoryCall> listed;
  std::vector<EntryCall> deleted;
  std::vector<EntryCall> renamed;
  std::vector<EntryCall> created_directories;
  std::vector<TransferCall> moved;
  std::vector<TransferCall> copied;

  struct ExportCall final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring name;
    std::wstring destination;
  };

  void export_container_entry(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name,
      const std::wstring_view absolute_destination_directory) override {
    exported_entries.push_back(
        {.id = id,
         .directory = std::wstring(relative_directory),
         .name = std::wstring(name),
         .destination = std::wstring(absolute_destination_directory)});
  }

  void export_container_file(
      const std::uint32_t id,
      const std::wstring_view absolute_destination_directory) override {
    exported_files.push_back(
        {.id = id,
         .destination = std::wstring(absolute_destination_directory)});
  }

  void export_container_folder(
      const std::uint32_t id,
      const std::wstring_view absolute_destination_directory,
      const std::wstring_view folder_name) override {
    exported_files.push_back(
        {.id = id,
         .name = std::wstring(folder_name),
         .destination = std::wstring(absolute_destination_directory)});
  }

  std::vector<ExportCall> exported_entries;
  std::vector<ExportCall> exported_files;

  void export_container_entry_encrypted(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name,
      const std::wstring_view absolute_destination_directory,
      const std::wstring_view package_name,
      const usb_vault::security::SecureBuffer& password,
      const std::function<bool()>&,
      const std::function<void(std::uint64_t, std::uint64_t)>&) override {
    encrypted_exports.push_back(
        {.id = id,
         .directory = std::wstring(relative_directory),
         .name = std::wstring(name),
         .destination = std::wstring(absolute_destination_directory) + L"|" +
                        std::wstring(package_name)});
    encrypted_password.assign(password.bytes().begin(), password.bytes().end());
  }
  std::vector<ExportCall> encrypted_exports;
  std::vector<std::byte> encrypted_password;

  std::wstring import_package_into_container(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view package,
      const usb_vault::security::SecureBuffer& password,
      const std::function<bool()>&,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) override {
    imports.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .destination = std::wstring(package)});
    encrypted_password.assign(password.bytes().begin(), password.bytes().end());
    return L"Docs";
  }
  std::vector<ExportCall> imports;

  std::wstring write_verified_import(
      const std::uint32_t id, const std::wstring_view relative_directory,
      usb_vault::backup::ImportPackageV1&, const usb_vault::security::SecureBuffer&,
      const std::function<bool()>&, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) override {
    import_events.push_back(L"write:" + std::to_wstring(id) + L":" + std::wstring(relative_directory));
    if (fail_import_write) throw std::runtime_error("the import write failed");
    return L"Docs";
  }
  void defer_operation_records() override { import_events.push_back(L"defer"); }
  void settle_operation_records(const bool keep) override {
    import_events.push_back(keep ? L"settle:keep" : L"settle:drop");
  }
  void discard_new_container(const std::uint32_t id) override {
    import_events.push_back(L"discard:" + std::to_wstring(id));
    if (fail_discard) throw std::runtime_error("the discard failed");
  }
  bool fail_discard{};
  std::vector<std::wstring> import_events;
  bool fail_import_write{};

  void rename_container(const std::uint32_t id, const std::wstring_view name) override {
    for (Adopted& entry : containers) {
      if (entry.id == id) entry.name.assign(name);
    }
  }

  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    ++container_statuses_calls;
    std::vector<usb_vault::application::ContainerStatus> result;
    result.reserve(containers.size());
    for (const Adopted& entry : containers) {
      result.push_back(
          {.id = entry.id,
           .name = entry.name,
           .mounted = entry.mounted,
           .size_bytes = entry.mounted && !zero_mounted_size ? entry.size_bytes : 0,
           .drive_letter = entry.mounted ? entry.drive_letter
                                         : inconsistent_unmounted_drive,
           .volume_unavailable = entry.mounted && flag_unavailable});
    }
    return result;
  }

  [[nodiscard]] ContainerAStatus container_a_status() override {
    ++container_a_status_calls;
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_size_bytes,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  [[nodiscard]] std::vector<usb_vault::application::OperationEvent>
  operation_history() override {
    ++operation_history_calls;
    return operation_events;
  }

  void clear_operation_history() noexcept override {
    ++clear_operation_history_calls;
    operation_events.clear();
  }

  void consume_authenticated_login(SecureBuffer root,
                                   SecureBuffer login_password_utf8) override {
    ++consume_calls;
    login_bytes.assign(login_password_utf8.bytes().begin(),
                       login_password_utf8.bytes().end());
    authentication_.establish(std::move(root));
    login_password_utf8.clear();
    if (fail_after_authentication) {
      throw std::runtime_error("post-authentication mount failed");
    }
    mounted_state = mount_on_success;
  }

  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) throw std::runtime_error("unmount failed");
    container_a_mounted = false;
    mounted_state = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("forced unmount is outside the UI contract");
  }

  void release_session_secrets() noexcept override {
    ++release_secrets_calls;
  }

  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  struct Adopted final {
    std::uint32_t id{};
    std::wstring path;
    std::wstring name;
    std::uint64_t size_bytes{};
    wchar_t drive_letter{};
    bool mounted{};
  };

  static constexpr std::uint64_t kFakeContainerBytes =
      64ULL * 1024ULL * 1024ULL;

  std::uint32_t adopt(const std::wstring_view path,
                      const std::uint64_t size_bytes) {
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
                                       ? path
                                       : path.substr(separator + 1);
    const std::uint32_t id = next_id++;
    containers.push_back(
        {.id = id,
         .path = std::wstring(path),
         .name = std::wstring(name),
         .size_bytes = size_bytes,
         .drive_letter = static_cast<wchar_t>(
             L'D' + static_cast<wchar_t>(containers.size())),
         .mounted = true});
    mounted_state = true;
    return id;
  }

  [[nodiscard]] bool any_mounted() const noexcept {
    for (const Adopted& entry : containers) {
      if (entry.mounted) return true;
    }
    return false;
  }

  CardAuthenticationSession& authentication_;
  std::vector<std::byte> login_bytes;
  std::vector<Adopted> containers;
  std::vector<usb_vault::application::OperationEvent> operation_events;
  std::wstring created_display_name;
  std::wstring created_b_display_name;
  // Product creation names the container apart from its private path.
  std::uint32_t create_protected_container(const std::wstring_view path,
      const std::uint64_t size_bytes, const std::wstring_view display_name) override {
    created_display_name.assign(display_name);
    const std::uint32_t id = create_container(path, size_bytes);
    containers.back().name.assign(display_name);
    return id;
  }
  std::uint32_t create_protected_container_b(const std::wstring_view path,
      const std::uint64_t outer_size_bytes, const std::uint64_t hidden_size_bytes,
      const SecureBuffer& decoy_password_utf8, const std::wstring_view display_name) override {
    created_b_display_name.assign(display_name);
    const std::uint32_t id = create_container_b(path, outer_size_bytes, hidden_size_bytes,
                                                decoy_password_utf8);
    containers.back().name.assign(display_name);
    return id;
  }
  std::wstring created_path;
  std::wstring mounted_path;
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  // Lets one test hand the controller an unmounted container that still
  // reports a drive letter, which must fail closed instead of being shown.
  wchar_t inconsistent_unmounted_drive{};
  bool zero_mounted_size{};
  bool flag_unavailable{};
  bool reject_create{};
  bool reject_create_b{};
  bool reject_mount{};
  std::wstring created_b_path;
  std::uint64_t created_outer_bytes{};
  std::uint64_t created_hidden_bytes{};
  void complete_initialization(SecureBuffer password) override {
    ++initialization_handoffs;
    if (reject_initialization_handoff) throw std::runtime_error("handoff failed");
    session_password.assign(password.bytes().begin(), password.bytes().end());
  }
  int initialization_handoffs{};
  bool reject_initialization_handoff{};
  SecureBuffer copy_session_password() override {
    ++session_password_calls;
    SecureBuffer copy(session_password.size());
    for (std::size_t index = 0; index < session_password.size(); ++index) {
      copy.bytes()[index] = session_password[index];
    }
    return copy;
  }

  std::vector<std::byte> b_decoy_password;
  unsigned int survey_calls{};
  unsigned int open_maintenance_calls{};
  std::uint32_t opened_maintenance_id{};
  usb_vault::application::DecoyMaintenanceSpace maintenance_space;
  unsigned int maintenance_space_calls{};
  unsigned int end_maintenance_calls{};
  std::uint32_t maintenance_space_id{};
  std::uint32_t ended_maintenance_id{};
  bool reject_maintenance_space{};
  std::vector<std::byte> survey_password;
  std::vector<std::uint32_t> survey_result;
  std::uint32_t survey_unreachable{};
  bool reject_survey{};
  bool reject_open_maintenance{};
  std::vector<std::byte> session_password{bytes("login-password")};
  int session_password_calls{};
  int create_b_calls{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};
  int forget_container_calls{};
  int mount_adopted_calls{};
  int container_statuses_calls{};
  int operation_history_calls{};
  int clear_operation_history_calls{};
  bool fail_after_authentication{};
  bool mount_on_success{};
  bool mounted_state{};
  bool reject_unmount{};
  bool reject_create_a{};
  bool container_a_exists{};
  bool container_a_mounted{};
  std::uint64_t container_a_size_bytes{};
  std::uint64_t created_a_size_bytes{};
  int consume_calls{};
  int create_a_calls{};
  int container_a_status_calls{};
  int unmount_calls{};
  int release_secrets_calls{};
};

class FakeEntry final : public ICardEntryActions {
 public:
  void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      ConfirmedPassword confirmed_daily_password) override {
    ++initialization_calls;
    SecureBuffer password = std::move(confirmed_daily_password).take();
    initialization_bytes.assign(password.bytes().begin(),
                                password.bytes().end());
    password.clear();
    route = CardEntryRoute::unavailable;
    authentication.establish(root_secret());
  }

  void authenticate_login(
      usb_vault::application::ICardLoginSuccessConsumer& success_consumer,
      SecureBuffer login_password_utf8) override {
    ++login_calls;
    if (reject_credential) {
      throw usb_vault::card::CardCredentialRejected();
    }
    if (fail_system) {
      route = CardEntryRoute::unavailable;
      throw std::system_error(
          usb_vault::card::make_error_code(
              usb_vault::card::CardError::device_failure));
    }
    route = CardEntryRoute::unavailable;
    success_consumer.consume_authenticated_login(
        root_secret(), std::move(login_password_utf8));
  }

  void set_second_card_password(SecureBuffer existing_password_utf8,
                                SecureBuffer second_password_utf8) override {
    ++second_password_calls;
    existing_password_bytes.assign(existing_password_utf8.bytes().begin(),
                                   existing_password_utf8.bytes().end());
    second_password_bytes.assign(second_password_utf8.bytes().begin(),
                                 second_password_utf8.bytes().end());
    existing_password_utf8.clear();
    second_password_utf8.clear();
    if (reject_second_password) {
      throw std::runtime_error("card record update failed");
    }
  }

  void reset_card_record(
      const usb_vault::application::IVolatileCardAuthenticationSession&
          authentication) override {
    ++reset_calls;
    if (!authentication.authenticated()) {
      throw std::runtime_error("card reset was not authenticated");
    }
    if (reject_reset) throw std::runtime_error("card reset failed");
    route = CardEntryRoute::unavailable;
  }

  std::uint32_t prepare_login_reset() override { reset_ticket = ++reset_serial; return reset_ticket; }
  void cancel_login_reset() noexcept override { reset_ticket = 0; }
  bool login_reset_pending(std::uint32_t ticket) const noexcept override {
    return ticket != 0 && ticket == reset_ticket && route == CardEntryRoute::login_required;
  }
  void confirm_login_reset(std::uint32_t ticket) override {
    if (!login_reset_pending(ticket)) throw std::runtime_error("expired");
    reset_ticket = 0; ++login_reset_calls; route = CardEntryRoute::unavailable;
  }
  std::uint32_t reset_ticket{}, reset_serial{};
  int login_reset_calls{};
  int reset_calls{};
  bool reject_reset{};

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }
  // Like the real controller, the identity lives only while an insertion
  // that can still initialize or log in is bound.
  [[nodiscard]] std::optional<std::uint32_t>
  current_card_identity() const noexcept override {
    if (route != CardEntryRoute::initialization_required &&
        route != CardEntryRoute::login_required) return std::nullopt;
    return identity;
  }

  std::optional<CardEntryRoute> route;
  std::optional<std::uint32_t> identity;
  std::vector<std::byte> initialization_bytes;
  bool reject_credential{};
  bool reject_second_password{};
  std::vector<std::byte> existing_password_bytes;
  std::vector<std::byte> second_password_bytes;
  int second_password_calls{};
  bool fail_system{};
  int initialization_calls{};
  int login_calls{};
};

// Creation no longer selects anything: the caller names the container and the
// directory. Only the browse button and the adoption dialog reach the chooser,
// and the counters prove when each was consulted.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  [[nodiscard]] std::wstring choose_container_directory() override {
    ++directory_calls;
    return directory_path;
  }

  [[nodiscard]] std::wstring choose_existing_container() override {
    ++existing_calls;
    return existing_path;
  }

  std::wstring directory_path;
  std::wstring existing_path;
  int directory_calls{};
  int existing_calls{};
};

// A save location has to exist now, so the tests need a real directory rather
// than a plausible-looking string.
class ScopedDirectory final {
 public:
  explicit ScopedDirectory(const wchar_t* const leaf)
      : path_(std::filesystem::temp_directory_path() /
              L"usb-vault-ui-tests" / leaf) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_);
  }

  ~ScopedDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;

  [[nodiscard]] std::wstring wide() const { return path_.wstring(); }

  // Whether `path` is a protected container directly inside this directory:
  // a 64-character lowercase hexadecimal folder holding a braced GUID file.
  [[nodiscard]] bool holds_protected(const std::wstring_view path) const {
    const std::wstring prefix = wide() + L"\\";
    if (!path.starts_with(prefix) || path.size() != prefix.size() + 64 + 1 + 38) return false;
    const std::wstring_view rest = path.substr(prefix.size());
    return rest.substr(0, 64).find_first_not_of(L"0123456789abcdef") == std::wstring_view::npos &&
           rest[64] == L'\\' && rest[65] == L'{' && rest.back() == L'}' &&
           rest.substr(66, 36).find_first_not_of(L"0123456789ABCDEF-") == std::wstring_view::npos;
  }

 private:
  std::filesystem::path path_;
};

std::uint32_t g_fixture_serial = 0;

class FakeResetMounts final : public usb_vault::ui::ILoginResetMountAccess {
 public:
  std::vector<usb_vault::volume::MountIdentity> values;
  bool query_fails{}, force_fails{};
  int forced{};
  std::vector<usb_vault::volume::MountIdentity> mounted() override {
    if (query_fails) throw std::runtime_error("driver unavailable");
    return values;
  }
  void force_unmount(const usb_vault::volume::MountIdentity& expected) override {
    if (force_fails) throw std::runtime_error("busy");
    ++forced;
    std::erase_if(values, [&](const auto& value) { return value == expected; });
  }
};
class FakeActivationGate final : public usb_vault::application::IActivationGate {
 public:
  [[nodiscard]] ActivationState state(const std::uint32_t card) noexcept override {
    const auto found = states.find(card);
    return found == states.end() ? ActivationState::missing : found->second;
  }
  [[nodiscard]] std::optional<std::int64_t> expiry(const std::uint32_t card) noexcept override {
    return states.contains(card) ? std::optional<std::int64_t>(1793375999) : std::nullopt;
  }
  [[nodiscard]] ActivationInstallResult install(const std::uint32_t card,
                                                const std::string_view code) override {
    installs.emplace_back(card, std::string(code));
    if (code != "GOOD") return ActivationInstallResult::invalid;
    states[card] = ActivationState::active;
    return ActivationInstallResult::accepted;
  }
  std::map<std::uint32_t, ActivationState> states;
  std::vector<std::pair<std::uint32_t, std::string>> installs;
};

struct Fixture final {
  Fixture()
      : directory(std::to_wstring(++g_fixture_serial).c_str()),
        volume(authentication),
        controller(entry, authentication, volume, policy, chooser, &reset_mounts) {}

  FakeEntry entry;
  ScopedDirectory directory;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume;
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  FakeResetMounts reset_mounts;
  CardProductUiSessionController controller;
};

template <typename Exception, typename Action>
void require_throws(Action&& action, const char* const message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

void test_state_projection_is_complete_and_authentication_wins() {
  Fixture fixture;
  require(fixture.controller.state() == CardProductUiState::no_card,
          "empty route did not map to no-card state");

  for (const auto [route, expected] : {
           std::pair{CardEntryRoute::initialization_required,
                     CardProductUiState::initialization_required},
           std::pair{CardEntryRoute::login_required,
                     CardProductUiState::login_required},
           std::pair{CardEntryRoute::unavailable,
                     CardProductUiState::unavailable}}) {
    fixture.entry.route = route;
    require(fixture.controller.state() == expected,
            "entry route mapped to the wrong UI state");
  }

  fixture.authentication.establish(root_secret());
  fixture.entry.route = CardEntryRoute::login_required;
  require(fixture.controller.state() == CardProductUiState::authenticated,
          "authenticated state did not take precedence over stale route");

  fixture.authentication.clear_authentication();
  fixture.volume.mounted_state = true;
  require(fixture.controller.state() == CardProductUiState::unavailable,
          "residual mount exposed a new-card action route");
}

void test_activation_gates_card_actions_and_survives_expiry_in_session() {
  Fixture plain;
  plain.entry.route = CardEntryRoute::login_required;
  require(plain.controller.state() == CardProductUiState::login_required &&
              plain.controller.activation_state() == ActivationState::active,
          "a controller without a gate was not treated as activated");
  require_throws<std::system_error>(
      [&] { static_cast<void>(plain.controller.submit_activation_code("GOOD")); },
      "activation was accepted without a gate");

  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume(authentication);
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  FakeActivationGate gate;
  CardProductUiSessionController controller(entry, authentication, volume, policy,
                                            chooser, nullptr, &gate);
  require(controller.state() == CardProductUiState::no_card,
          "no card did not stay no-card under a gate");
  entry.route = CardEntryRoute::unavailable;
  require(controller.state() == CardProductUiState::unavailable,
          "an unavailable card was offered activation");

  entry.route = CardEntryRoute::login_required;
  require(controller.state() == CardProductUiState::activation_required,
          "a card without an identity was not gated");
  entry.identity = 0x1A2B3C4D;
  for (const auto route : {CardEntryRoute::login_required,
                           CardEntryRoute::initialization_required}) {
    entry.route = route;
    require(controller.state() == CardProductUiState::activation_required,
            "an unactivated card reached login or initialization");
  }
  entry.route = CardEntryRoute::login_required;
  auto blocked = bytes("password-1");
  require_throws<std::system_error>([&] { controller.submit_login(blocked); },
                                    "login bypassed the activation gate");
  require_wiped(blocked, "a gated login input was not wiped");
  require(entry.login_calls == 0, "a gated login reached the card");

  require(controller.submit_activation_code("BAD") == ActivationInstallResult::invalid &&
              controller.state() == CardProductUiState::activation_required,
          "an invalid code changed the route");
  require(controller.submit_activation_code("GOOD") == ActivationInstallResult::accepted &&
              gate.installs.back().first == 0x1A2B3C4D &&
              controller.state() == CardProductUiState::login_required,
          "a valid code did not open the login route for this card");

  auto input = bytes("password-1");
  controller.submit_login(input);
  require(controller.state() == CardProductUiState::authenticated &&
              !entry.current_card_identity() &&
              controller.activation_card() == 0x1A2B3C4DU,
          "the session did not keep its card identity after login");
  gate.states[0x1A2B3C4D] = ActivationState::expired;
  require(controller.state() == CardProductUiState::authenticated &&
              controller.activation_state() == ActivationState::expired &&
              controller.activation_expiry() == 1793375999,
          "expiry ended a running session or was not reported");
  require(controller.submit_activation_code("GOOD") == ActivationInstallResult::accepted &&
              gate.installs.back().first == 0x1A2B3C4D,
          "renewal inside the session targeted the wrong card");
}

void test_login_normalizes_wipes_and_preserves_retry_semantics() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  std::vector<std::byte> input{
      std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}};

  fixture.controller.submit_login(input);

  const std::array expected{std::byte{0xC3}, std::byte{0xA9}};
  require_wiped(input, "login bridge input was not wiped");
  require(equals(fixture.volume.login_bytes, expected) &&
              fixture.entry.login_calls == 1 &&
              fixture.volume.consume_calls == 1 &&
              fixture.controller.state() == CardProductUiState::authenticated,
          "login did not normalize, forward, or establish UI authentication");

  Fixture rejected;
  rejected.entry.route = CardEntryRoute::login_required;
  rejected.entry.reject_credential = true;
  auto rejected_input = bytes("wrong-password");
  require_throws<usb_vault::card::CardCredentialRejected>(
      [&] { rejected.controller.submit_login(rejected_input); },
      "credential rejection was not propagated");
  require_wiped(rejected_input, "rejected login input was not wiped");
  require(rejected.controller.state() == CardProductUiState::login_required &&
              !rejected.authentication.authenticated(),
          "credential rejection lost its retryable UI route");
}

void test_post_authentication_failure_projects_authenticated_state() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  fixture.volume.fail_after_authentication = true;
  auto input = bytes("valid-password");

  require_throws<std::runtime_error>(
      [&] { fixture.controller.submit_login(input); },
      "post-authentication failure was hidden");
  require_wiped(input, "failed post-authentication input was not wiped");
  require(fixture.controller.state() == CardProductUiState::authenticated &&
              !fixture.controller.mounted(),
          "post-authentication failure projected a login failure or mount");
}

void test_initialization_normalizes_both_inputs_and_creates_proof() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  std::vector<std::byte> password = bytes("aaaaaaaaaaae");
  password.push_back(std::byte{0xCC});
  password.push_back(std::byte{0x81});
  std::vector<std::byte> confirmation = bytes("aaaaaaaaaaa");
  confirmation.push_back(std::byte{0xC3});
  confirmation.push_back(std::byte{0xA9});

  fixture.controller.submit_initialization(password, confirmation);

  std::vector<std::byte> expected = bytes("aaaaaaaaaaa");
  expected.push_back(std::byte{0xC3});
  expected.push_back(std::byte{0xA9});
  require(fixture.volume.initialization_handoffs == 1 &&
              equals(fixture.volume.session_password, expected),
          "initialization did not hand the normalized password to the session");
  require_wiped(password, "initialization password was not wiped");
  require_wiped(confirmation, "initialization confirmation was not wiped");
  require(equals(fixture.entry.initialization_bytes, expected) &&
              fixture.entry.initialization_calls == 1 &&
              fixture.controller.state() == CardProductUiState::authenticated,
          "initialization did not normalize, prove, or authenticate");
}

void test_initialization_immediately_supports_hidden_creation() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  fixture.volume.session_password.clear();
  auto password = bytes("new-login-password-value");
  auto confirmation = bytes("new-login-password-value");
  fixture.controller.submit_initialization(password, confirmation);
  auto second = bytes("second-password-value");
  auto second_confirmation = bytes("second-password-value");
  fixture.controller.create_container_b(L"after-init", fixture.directory.wide(),
      64, 16, second, second_confirmation);
  require(fixture.entry.login_calls == 0 && fixture.volume.initialization_handoffs == 1 &&
              fixture.entry.second_password_calls == 1 && fixture.volume.create_b_calls == 1 &&
              equals(fixture.entry.existing_password_bytes, bytes("new-login-password-value")),
          "initialization did not support hidden creation without another login");
  require_wiped(password, "initialization input survived hidden creation");
  require_wiped(second, "second input survived hidden creation");
}

void test_failed_initialization_handoff_clears_authentication() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  fixture.volume.session_password.clear();
  fixture.volume.reject_initialization_handoff = true;
  auto password = bytes("new-login-password-value");
  auto confirmation = bytes("new-login-password-value");
  require_throws<std::runtime_error>([&] {
    fixture.controller.submit_initialization(password, confirmation);
  }, "failed session handoff reported initialization success");
  require(!fixture.authentication.authenticated() && fixture.volume.session_password.empty(),
          "failed handoff retained incomplete authentication");
  require_wiped(password, "failed handoff retained page password");
  require_wiped(confirmation, "failed handoff retained confirmation");
}

void test_initialization_rejections_wipe_both_before_entry_action() {
  for (const bool mismatch : {false, true}) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    auto password = bytes(mismatch ? "aaaaaaaaaaaa" : "short");
    auto confirmation = bytes(mismatch ? "bbbbbbbbbbbb" : "short");

    require_throws<usb_vault::security::PasswordConfirmationRejected>(
        [&] {
          fixture.controller.submit_initialization(password, confirmation);
        },
        "initialization rejection was not propagated");
    require_wiped(password, "rejected initialization password was not wiped");
    require_wiped(confirmation,
                  "rejected initialization confirmation was not wiped");
    require(fixture.entry.initialization_calls == 0 &&
                fixture.controller.state() ==
                    CardProductUiState::initialization_required,
            "rejected initialization reached the card or changed route");
  }

  Fixture invalid_ingress;
  invalid_ingress.entry.route = CardEntryRoute::initialization_required;
  std::vector<std::byte> invalid{std::byte{0xFF}};
  auto untouched_confirmation = bytes("aaaaaaaaaaaa");
  require_throws<std::invalid_argument>(
      [&] {
        invalid_ingress.controller.submit_initialization(
            invalid, untouched_confirmation);
      },
      "invalid first UTF-8 input was accepted");
  require_wiped(invalid, "invalid first input was not wiped");
  require_wiped(untouched_confirmation,
                "second input survived first-ingress failure");
}

void test_wrong_route_wipes_without_secret_processing() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::unavailable;
  auto login = bytes("must-be-wiped");
  require_throws<std::system_error>(
      [&] { fixture.controller.submit_login(login); },
      "login was accepted on the unavailable route");
  require_wiped(login, "wrong-route login was not wiped");

  auto password = bytes("aaaaaaaaaaaa");
  auto confirmation = bytes("aaaaaaaaaaaa");
  require_throws<std::system_error>(
      [&] {
        fixture.controller.submit_initialization(password, confirmation);
      },
      "initialization was accepted on the unavailable route");
  require_wiped(password, "wrong-route initialization password survived");
  require_wiped(confirmation,
                "wrong-route initialization confirmation survived");
  require(fixture.entry.login_calls == 0 &&
              fixture.entry.initialization_calls == 0,
          "wrong-route input reached an entry action");
}

void test_overlapping_initialization_inputs_are_rejected_and_wiped() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  std::vector<std::byte> shared = bytes("abcdefghijklmnop");
  const std::span<std::byte> password(shared.data(), 12);
  const std::span<std::byte> confirmation(shared.data() + 4, 12);

  require_throws<std::invalid_argument>(
      [&] {
        fixture.controller.submit_initialization(password, confirmation);
      },
      "overlapping initialization inputs were accepted");
  require_wiped(shared, "overlapping initialization storage was not wiped");
  require(fixture.entry.initialization_calls == 0,
          "overlapping initialization inputs reached the card");
}

void test_residual_mount_blocks_new_card_actions_until_cleanup() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  fixture.volume.mounted_state = true;
  auto input = bytes("must-be-wiped");

  require_throws<std::system_error>(
      [&] { fixture.controller.submit_login(input); },
      "new login was accepted over a residual mount");
  require_wiped(input, "residual-mount login input was not wiped");
  require(fixture.entry.login_calls == 0 &&
              fixture.entry.route == CardEntryRoute::login_required,
          "residual mount consumed the fresh card route");

  fixture.controller.unmount();
  require(fixture.controller.state() == CardProductUiState::login_required,
          "cleanup did not reveal the preserved fresh-card route");
}

void test_unmount_forwards_only_unified_active_state() {
  Fixture fixture;
  fixture.volume.mounted_state = true;
  require(fixture.controller.mounted(),
          "controller hid unified active-volume state");
  fixture.controller.unmount();
  require(!fixture.controller.mounted() && fixture.volume.unmount_calls == 1,
          "controller did not forward unmount once");
}

void test_container_collection_is_projected_only_when_authenticated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  require(fixture.controller.container_statuses().empty() &&
              fixture.volume.container_statuses_calls == 0,
          "unauthenticated UI queried or exposed the container collection");

  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);

  std::vector<ContainerUiStatus> statuses =
      fixture.controller.container_statuses();
  require(statuses.size() == 1 && statuses[0].id != 0 &&
              statuses[0].name == L"work" && statuses[0].mounted &&
              statuses[0].size_mib == 64 && statuses[0].drive_letter == L'D',
          "container projection lost identity, name, capacity, or drive");
  // The typed name is only shown; the file lands under random names in the
  // chosen directory, joined here rather than on the page.
  require(fixture.directory.holds_protected(fixture.volume.created_path) &&
              fixture.volume.created_display_name == L"work" &&
              fixture.volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL,
          "the named path or capacity did not reach the session");
  require(fixture.chooser.directory_calls == 0,
          "creating a container opened a dialog the form had replaced");

  fixture.controller.unmount_container(statuses[0].id);
  statuses = fixture.controller.container_statuses();
  require(statuses.size() == 1 && !statuses[0].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              statuses[0].size_mib == 0,
          "an unmounted container kept a drive letter or a capacity");
}

void test_a_location_without_room_is_refused_before_anything_is_written() {
  // A container is written at its full size, so a location that cannot hold it
  // used to fail inside the format, after a partial file existed and after the
  // user had waited. It has to be refused first, and refused by name.
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::wstring directory = fixture.directory.wide();
  const std::uint64_t free_bytes =
      usb_vault::volume::writable_free_bytes(directory);
  const std::uint64_t free_mib = free_bytes / (1024ULL * 1024ULL);
  if (free_mib + 1 > kContainerCreationMaximumMib) {
    // Nothing this machine can ask for exceeds what it has free, so there is
    // no honest way to exercise the refusal here.
    std::cout << "  (skipped: the save location has more room than the "
                 "largest container the UI allows)\n";
    return;
  }
  const auto too_large = static_cast<std::uint32_t>(free_mib + 1);

  const auto refused_as_full = [&](auto&& action) {
    try {
      action();
    } catch (const std::system_error& error) {
      return error.code() ==
             usb_vault::volume_creation::make_error_code(
                 usb_vault::volume_creation::CreationError::destination_full);
    } catch (...) {
      return false;
    }
    return false;
  };

  require(refused_as_full([&] {
            fixture.controller.create_container(L"work", directory, too_large);
          }),
          "a container larger than the location was not refused as such");
  require(std::filesystem::is_empty(fixture.directory.wide()),
          "a refused creation still left a file behind");
  require(fixture.volume.create_calls == 0,
          "a refused creation still reached the volume session");

  // The hidden container is measured by its outer size: the hidden volume
  // lives inside that file and costs the location nothing extra.
  require(refused_as_full([&] {
            auto decoy = bytes("decoy-password");
            auto confirmation = bytes("decoy-password");
            fixture.controller.create_container_b(L"decoy", directory, too_large,
                                                  too_large / 2, decoy,
                                                  confirmation);
          }),
          "a hidden container larger than the location was not refused as such");

  // A size the location can hold still passes this gate.
  require(!refused_as_full([&] {
            fixture.controller.create_container(L"fits", directory,
                                                kContainerCreationMinimumMib);
          }),
          "a container that fits was refused for space");
}

void test_container_creation_is_bounded_and_validated() {
  Fixture fixture;
  const std::wstring directory = fixture.directory.wide();
  fixture.entry.route = CardEntryRoute::login_required;
  require_throws<std::system_error>(
      [&] { fixture.controller.create_container(L"work", directory, 64); },
      "unauthenticated route created a container");

  fixture.authentication.establish(root_secret());
  for (const std::uint32_t invalid : {kContainerCreationMinimumMib - 1,
                                      kContainerCreationMaximumMib + 1}) {
    require_throws<std::invalid_argument>(
        [&] {
          fixture.controller.create_container(L"work", directory, invalid);
        },
        "UI accepted a container size outside the shared range");
  }

  // The page owns these two strings now, so every shape it must not be able to
  // express has to be refused here rather than somewhere downstream. The
  // syntax rules are covered exhaustively by the path unit's own tests; what
  // matters here is that this boundary applies them at all, before the session
  // is touched.
  for (const std::wstring_view rejected_name :
       {std::wstring_view(L""), std::wstring_view(L"..\\escape"),
        std::wstring_view(L"work.hc:stream"), std::wstring_view(L"NUL"),
        std::wstring_view(L"trailing ")}) {
    require_throws<std::invalid_argument>(
        [&] {
          fixture.controller.create_container(rejected_name, directory, 64);
        },
        "UI accepted a container name a file name cannot carry");
  }
  for (const std::wstring_view rejected_directory :
       {std::wstring_view(L""), std::wstring_view(L"Vault"),
        std::wstring_view(L"\\\\server\\share"),
        std::wstring_view(L"\\\\?\\D:\\Vault"),
        std::wstring_view(L"D:\\Vault\\..\\Windows")}) {
    require_throws<std::invalid_argument>(
        [&] {
          fixture.controller.create_container(L"work", rejected_directory, 64);
        },
        "UI accepted a save location the form must not be able to name");
  }
  // A well-formed location that is absent is a rejected field too: the
  // directory is never created on the user's behalf.
  require_throws<std::invalid_argument>(
      [&] {
        fixture.controller.create_container(
            L"work", directory + L"\\missing", 64);
      },
      "UI accepted a save location that does not exist");
  require(fixture.volume.create_calls == 0,
          "a rejected request still reached the session");

  fixture.volume.reject_create = true;
  require_throws<std::runtime_error>(
      [&] { fixture.controller.create_container(L"work", directory, 64); },
      "a failed creation was not reported");
  require(fixture.controller.state() == CardProductUiState::authenticated,
          "a failed creation dropped the authenticated route");
  require(fixture.controller.container_statuses().empty(),
          "a failed creation left an adopted container behind");
  fixture.volume.reject_create = false;

  fixture.controller.create_container(L"work", directory, 64);
  require(fixture.volume.create_calls == 2 &&
              fixture.controller.container_statuses().size() == 1,
          "a valid request after a rejected one did not go through");
}

// The browse button is the only place creation still opens a dialog, and what
// it returns is a value for a field rather than a decision.
void test_browsing_for_a_directory_only_reports_the_choice() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  require_throws<std::system_error>(
      [&] {
        static_cast<void>(fixture.controller.browse_for_container_directory());
      },
      "an unauthenticated route opened the directory picker");
  require(fixture.chooser.directory_calls == 0,
          "an unauthenticated route still reached the chooser");

  fixture.authentication.establish(root_secret());
  fixture.chooser.directory_path = L"D:\\Vault";
  require(fixture.controller.browse_for_container_directory() ==
                  L"D:\\Vault" &&
              fixture.chooser.directory_calls == 1,
          "the chosen directory did not reach the caller");

  fixture.chooser.directory_path.clear();
  require(fixture.controller.browse_for_container_directory().empty() &&
              fixture.chooser.directory_calls == 2,
          "a cancelled picker was not reported as an empty choice");
  require(fixture.volume.create_calls == 0,
          "browsing created something on its own");
}

void test_adopted_container_remounts_without_reselecting_its_file() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);
  const std::uint32_t id = fixture.controller.container_statuses()[0].id;

  fixture.controller.unmount_container(id);
  fixture.controller.mount_container(id);
  require(fixture.chooser.existing_calls == 0 &&
              fixture.volume.mount_adopted_calls == 1 &&
              fixture.volume.mount_calls == 0,
          "remounting an adopted container asked for the file again");
  require(fixture.controller.container_statuses()[0].mounted,
          "the remounted container is not reported as mounted");
  require_throws<std::invalid_argument>(
      [&] { fixture.controller.mount_container(id + 7); },
      "UI mounted an identifier the session never adopted");

  fixture.chooser.existing_path = L"E:\\Media\\photos.hc";
  require(fixture.controller.mount_container() &&
              fixture.chooser.existing_calls == 1 &&
              fixture.controller.container_statuses().size() == 2,
          "adopting a second container through the chooser failed");
  fixture.chooser.existing_path.clear();
  require(!fixture.controller.mount_container() &&
              fixture.controller.container_statuses().size() == 2,
          "a cancelled chooser still adopted a container");
}

void test_dropping_a_container_takes_the_record_and_not_the_file() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);
  // This case is about a container that is mounted, so the import has to be
  // the mounting kind rather than the default one.
  fixture.controller.set_mounting_imported_containers(true);
  fixture.chooser.existing_path = L"E:\\Media\\photos.hc";
  require(fixture.controller.mount_container(),
          "adopting a second container failed");
  fixture.chooser.existing_path.clear();

  const std::vector<ContainerUiStatus> before =
      fixture.controller.container_statuses();
  require(before.size() == 2, "the fixture did not produce two containers");
  const std::uint32_t dropped = before[1].id;

  // The session refuses a mounted one, and the page must not paper over that.
  require_throws<std::logic_error>(
      [&] { fixture.controller.forget_container(dropped); },
      "a mounted container was dropped from the list");

  fixture.controller.unmount_container(dropped);
  fixture.controller.forget_container(dropped);
  require(fixture.volume.forget_container_calls == 2,
          "the drop did not reach the session both times");

  const std::vector<ContainerUiStatus> after =
      fixture.controller.container_statuses();
  require(after.size() == 1 && after[0].id == before[0].id,
          "dropping one container did not leave exactly the other");
  require_throws<std::invalid_argument>(
      [&] { fixture.controller.forget_container(dropped); },
      "a container already off the list could be dropped again");
}

void test_importing_only_mounts_when_the_switch_asks_for_it() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.chooser.existing_path = L"D:\\vault\\imported.hc";

  // Off is the default: picking a file says where a container is, not that it
  // should be opened.
  require(!fixture.controller.mounting_imported_containers(),
          "importing offered to mount by default");
  require(fixture.controller.mount_container(),
          "the import was refused");
  require(fixture.volume.adopt_calls == 1 && fixture.volume.mount_calls == 0 &&
              fixture.volume.adopted_path == L"D:\\vault\\imported.hc",
          "a default import opened the container");

  fixture.controller.set_mounting_imported_containers(true);
  fixture.chooser.existing_path = L"D:\\vault\\second.hc";
  require(fixture.controller.mount_container(), "the second import failed");
  require(fixture.volume.mount_calls == 1 && fixture.volume.adopt_calls == 1 &&
              fixture.volume.mounted_path == L"D:\\vault\\second.hc",
          "the switch did not send the import through the mount");

  // A cancelled picker reaches neither call.
  fixture.chooser.existing_path.clear();
  require(!fixture.controller.mount_container(),
          "a cancelled picker reported an import");
  require(fixture.volume.mount_calls == 1 && fixture.volume.adopt_calls == 1,
          "a cancelled picker still reached the session");
}

void test_the_import_switch_reads_anywhere_and_changes_only_inside() {
  Fixture fixture;
  require(!fixture.controller.mounting_imported_containers(),
          "the switch did not read its default outside a session");
  require_throws<std::system_error>(
      [&] { fixture.controller.set_mounting_imported_containers(true); },
      "the switch was changed without an authenticated session");
  require(fixture.volume.mount_imported_writes == 0,
          "a wrong-route change still reached the session");

  fixture.authentication.establish(root_secret());
  fixture.controller.set_mounting_imported_containers(true);
  require(fixture.volume.mount_imported_writes == 1 &&
              fixture.controller.mounting_imported_containers(),
          "the switch did not change inside an authenticated session");
}

void test_the_remember_switch_reads_anywhere_and_changes_only_inside() {
  Fixture fixture;
  // The settings page shows the switch in its real position before anyone
  // logs in, so reading it cannot depend on a session.
  require(fixture.controller.remembering_container_paths(),
          "the switch did not read its default outside a session");
  require_throws<std::system_error>(
      [&] { fixture.controller.set_remembering_container_paths(false); },
      "the switch was changed without an authenticated session");
  require(fixture.volume.remember_paths_writes == 0,
          "a wrong-route change still reached the session");

  fixture.authentication.establish(root_secret());
  fixture.controller.set_remembering_container_paths(false);
  require(fixture.volume.remember_paths_writes == 1 &&
              !fixture.controller.remembering_container_paths(),
          "the switch did not change inside an authenticated session");
  fixture.controller.set_remembering_container_paths(true);
  require(fixture.controller.remembering_container_paths(),
          "the switch could not be turned back on");
}

void test_dropping_a_container_is_refused_off_the_authenticated_route() {
  Fixture fixture;
  require_throws<std::system_error>(
      [&] { fixture.controller.forget_container(1); },
      "a container was dropped without an authenticated session");
  require(fixture.volume.forget_container_calls == 0,
          "a wrong-route drop still reached the session");
}

void test_a_survey_passes_the_password_through_and_wipes_it() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.survey_result = {4, 9};
  fixture.volume.survey_unreachable = 2;
  auto password = bytes("decoy-password-value");

  const usb_vault::ui::DecoyMaintenanceUiSurvey survey =
      fixture.controller.survey_decoy_maintenance(password);

  require(fixture.volume.survey_calls == 1, "the survey did not run once");
  require(fixture.volume.survey_password.size() == 20,
          "the survey did not receive the password the page supplied");
  require(survey.maintainable.size() == 2 && survey.maintainable[0] == 4 &&
              survey.maintainable[1] == 9 && survey.unreachable == 2 &&
              !survey.cancelled,
          "the survey was not projected as the session reported it");
  for (const std::byte value : password) {
    require(value == std::byte{}, "the survey left the page's buffer intact");
  }
}

void test_a_survey_wipes_the_password_when_it_fails() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.reject_survey = true;
  auto password = bytes("decoy-password-value");
  bool threw = false;
  try {
    static_cast<void>(fixture.controller.survey_decoy_maintenance(password));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw, "a failed survey was reported as a success");
  for (const std::byte value : password) {
    require(value == std::byte{}, "a failed survey left the buffer intact");
  }
}

void test_maintenance_is_refused_outside_an_authenticated_session() {
  Fixture fixture;
  auto password = bytes("decoy-password-value");
  bool refused = false;
  try {
    static_cast<void>(fixture.controller.survey_decoy_maintenance(password));
  } catch (const std::system_error& error) {
    refused = error.code() ==
              usb_vault::card::make_error_code(
                  usb_vault::card::CardError::invalid_operation);
  }
  require(refused, "a survey ran without an authenticated session");
  require(fixture.volume.survey_calls == 0, "the session was reached anyway");
  for (const std::byte value : password) {
    require(value == std::byte{}, "a refused survey left the buffer intact");
  }

  refused = false;
  try {
    static_cast<void>(fixture.controller.open_decoy_maintenance(1));
  } catch (const std::system_error& error) {
    refused = error.code() ==
              usb_vault::card::make_error_code(
                  usb_vault::card::CardError::invalid_operation);
  }
  require(refused && fixture.volume.open_maintenance_calls == 0,
          "maintenance opened without an authenticated session");

  require_throws<std::system_error>(
      [&] {
        static_cast<void>(fixture.controller.decoy_maintenance_space(1));
      },
      "maintenance capacity was measured without an authenticated session");
  require_throws<std::system_error>(
      [&] { fixture.controller.end_decoy_maintenance(1); },
      "maintenance ended without an authenticated session");
  require(fixture.volume.maintenance_space_calls == 0 &&
              fixture.volume.end_maintenance_calls == 0,
          "a refused maintenance fact still reached the session");
}

void test_opening_maintenance_names_only_the_container() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const wchar_t drive_letter = fixture.controller.open_decoy_maintenance(7);
  require(drive_letter == L'Q' && fixture.volume.opened_maintenance_id == 7,
          "the page did not reach the container it named");
}

void test_maintenance_capacity_is_projected_conservatively_and_can_end() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  constexpr std::uint64_t mib = 1024ULL * 1024ULL;
  fixture.volume.maintenance_space = {
      .total_bytes = (static_cast<std::uint64_t>(UINT32_MAX) + 10ULL) * mib,
      .free_bytes = 9ULL * mib + mib / 2ULL,
      .tail_free_bytes = 0,
      .safe_free_bytes = 513,
  };

  const usb_vault::ui::DecoyMaintenanceUiSpace space =
      fixture.controller.decoy_maintenance_space(7);
  require(space.total_bytes == (static_cast<std::uint64_t>(UINT32_MAX) + 10ULL) * mib &&
              space.free_bytes == 9ULL * mib + mib / 2ULL && space.safe_free_bytes == 513,
          "maintenance byte facts lost precision or used legacy tail space");
  require(fixture.volume.maintenance_space_calls == 1 &&
              fixture.volume.maintenance_space_id == 7,
          "capacity was measured for a container the page did not name");

  fixture.controller.end_decoy_maintenance(7);
  require(fixture.volume.end_maintenance_calls == 1 &&
              fixture.volume.ended_maintenance_id == 7,
          "ending maintenance did not name only its container");
}

void test_partition_pair_controller() {
  for (int scenario=0; scenario<5; ++scenario) {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    auto second=bytes("second-password-value"), confirmation=bytes("second-password-value");
    usb_vault::volume::PartitionCandidate target;
    target.volume_path=LR"(\?\Volume{12345678-1234-1234-1234-123456789abc}\)";
    target.device_id[0]=7; target.disk_number=1; target.partition_number=1;
    target.layout=usb_vault::volume::PartitionLayout::mbr; target.logical_sector_bytes=512;
    target.offset_bytes=1048576; target.length_bytes=64ULL*1024*1024;
    target.availability=usb_vault::volume::PartitionAvailability::candidate;
    if(scenario==1)fixture.volume.remember_paths=false;
    if(scenario==2)confirmation[0]=std::byte{'x'};
    if(scenario==3)fixture.entry.reject_second_password=true;
    bool failed=false;
    try {fixture.controller.create_partition_pair(target,scenario==4 ? 64 : 16,second,confirmation,[](const auto&){return true;});}
    catch(...){failed=true;}
    require(failed==(scenario!=0),"partition controller outcome");
    require(fixture.volume.create_b_calls==(scenario==0 ? 1 : 0),"partition started without prerequisites");
    require(std::ranges::all_of(second,[](auto b){return b==std::byte{};}) &&
        std::ranges::all_of(confirmation,[](auto b){return b==std::byte{};}),"partition input not wiped");
  }
}

void test_container_b_commits_the_card_before_the_volume() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  auto second = bytes("second-password-value");
  auto confirmation = bytes("second-password-value");

  fixture.controller.create_container_b(
      L"handover", fixture.directory.wide(), 64, 16, second, confirmation);

  require(fixture.entry.second_password_calls == 1 &&
              fixture.volume.create_b_calls == 1,
          "container B did not run both halves exactly once");
  // The location is named on the form exactly as a standard container's is,
  // and it is resolved by the same rules. 8.7.2: neither shape asks for
  // anything the other does not.
  require(fixture.chooser.directory_calls == 0 &&
              fixture.directory.holds_protected(fixture.volume.created_b_path) &&
              fixture.volume.created_b_display_name == L"handover",
          "container B was not created at the named location");
  // The password that rewraps the card record comes from the session, not
  // from the page: the page never sent one.
  require(fixture.volume.session_password_calls == 1 &&
              fixture.entry.existing_password_bytes ==
                  bytes("login-password") &&
              fixture.entry.second_password_bytes ==
                  bytes("second-password-value"),
          "the card record received the wrong passwords");
  require(fixture.volume.b_decoy_password ==
              bytes("second-password-value"),
          "the volume transaction received the wrong password");
  require(fixture.volume.created_outer_bytes == 64ULL * 1024ULL * 1024ULL &&
              fixture.volume.created_hidden_bytes ==
                  16ULL * 1024ULL * 1024ULL,
          "container B was created with the wrong sizes");
  for (const std::vector<std::byte>& input : {second, confirmation}) {
    for (const std::byte value : input) {
      require(value == std::byte{}, "a container B input survived the call");
    }
  }

  // A session that no longer holds its login input stops before the card is
  // touched at all.
  Fixture released;
  released.authentication.establish(root_secret());
  released.volume.session_password.clear();
  auto released_second = bytes("second-password-value");
  auto released_confirmation = bytes("second-password-value");
  require_throws<std::system_error>(
      [&] {
        released.controller.create_container_b(
            L"handover", released.directory.wide(), 64, 16, released_second,
            released_confirmation);
      },
      "a session without its login input still created container B");
  require(released.entry.second_password_calls == 0 &&
              released.volume.create_b_calls == 0,
          "a released session still reached the card or volume");

  // A rejected location leaves the card untouched: the record is rewritten
  // only once the location has been resolved.
  Fixture rejected;
  rejected.authentication.establish(root_secret());
  auto rejected_second = bytes("second-password-value");
  auto rejected_confirmation = bytes("second-password-value");
  require_throws<std::invalid_argument>(
      [&] {
        rejected.controller.create_container_b(
            L"handover", L"Vault", 64, 16, rejected_second,
            rejected_confirmation);
      },
      "a rejected save location was reported as a creation");
  require(rejected.entry.second_password_calls == 0 &&
              rejected.volume.create_b_calls == 0,
          "a rejected location still rewrote the card or created a volume");
  for (const std::vector<std::byte>& input :
       {rejected_second, rejected_confirmation}) {
    for (const std::byte value : input) {
      require(value == std::byte{},
              "a rejected container B request kept its inputs");
    }
  }

  // A failed card record must stop before the volume exists, because a
  // container nobody can open with the second password blocks every retry.
  Fixture blocked;
  blocked.authentication.establish(root_secret());
  blocked.entry.reject_second_password = true;
  auto blocked_second = bytes("second-password-value");
  auto blocked_confirmation = bytes("second-password-value");
  require_throws<std::runtime_error>(
      [&] {
        blocked.controller.create_container_b(
            L"handover", blocked.directory.wide(), 64, 16, blocked_second,
            blocked_confirmation);
      },
      "a failed card record update was not reported");
  require(blocked.volume.create_b_calls == 0,
          "the volume was created after the card record failed");
}

void test_container_b_rejects_unusable_requests() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  auto second = bytes("second-password-value");
  auto confirmation = bytes("second-password-value");
  const std::wstring directory = fixture.directory.wide();
  require_throws<std::system_error>(
      [&] {
        fixture.controller.create_container_b(L"handover", directory, 64, 16,
                                              second, confirmation);
      },
      "an unauthenticated route created container B");

  fixture.authentication.establish(root_secret());
  // 48 of 64 MiB is the largest inner volume the three-quarter rule allows,
  // so 49 is the first one the boundary has to refuse.
  for (const auto [outer, hidden] : {
           std::pair<std::uint32_t, std::uint32_t>{19, 8},
           std::pair<std::uint32_t, std::uint32_t>{
               kContainerCreationMaximumMib + 1, 8},
           std::pair<std::uint32_t, std::uint32_t>{64, 0},
           std::pair<std::uint32_t, std::uint32_t>{64, 49},
           std::pair<std::uint32_t, std::uint32_t>{64, 64}}) {
    auto sized_second = bytes("second-password-value");
    auto sized_confirmation = bytes("second-password-value");
    require_throws<std::invalid_argument>(
        [&] {
          fixture.controller.create_container_b(L"handover", directory, outer,
                                                hidden, sized_second,
                                                sized_confirmation);
        },
        "container B accepted an unusable size pair");
  }

  // 8.7.2: the location is checked by the same rules on this shape as on the
  // standard one, and a rejection here reaches neither the card nor a volume.
  for (const std::wstring_view rejected_directory :
       {std::wstring_view(L""), std::wstring_view(L"Vault"),
        std::wstring_view(L"\\\\server\\share")}) {
    auto placed_second = bytes("second-password-value");
    auto placed_confirmation = bytes("second-password-value");
    require_throws<std::invalid_argument>(
        [&] {
          fixture.controller.create_container_b(
              L"handover", rejected_directory, 64, 16, placed_second,
              placed_confirmation);
        },
        "container B accepted a save location the form must not name");
  }
  require_throws<std::invalid_argument>(
      [&] {
        auto named_second = bytes("second-password-value");
        auto named_confirmation = bytes("second-password-value");
        fixture.controller.create_container_b(L"NUL", directory, 64, 16,
                                              named_second,
                                              named_confirmation);
      },
      "container B accepted a reserved device name");

  auto mismatched = bytes("second-password-value");
  auto other = bytes("different-password-val");
  require_throws<usb_vault::security::PasswordConfirmationRejected>(
      [&] {
        fixture.controller.create_container_b(L"handover", directory, 64, 16,
                                              mismatched, other);
      },
      "container B accepted two different second passwords");
  require(fixture.entry.second_password_calls == 0 &&
              fixture.volume.create_b_calls == 0 &&
              fixture.chooser.directory_calls == 0,
          "a rejected container B request still reached the card or volume");

  auto shared = bytes("second-password-value");
  require_throws<std::invalid_argument>(
      [&] {
        fixture.controller.create_container_b(L"handover", directory, 64, 16,
                                              shared, shared);
      },
      "container B accepted overlapping input buffers");

  // The largest inner volume the rule does allow still goes through.
  auto allowed_second = bytes("second-password-value");
  auto allowed_confirmation = bytes("second-password-value");
  fixture.controller.create_container_b(L"handover", directory, 64, 48,
                                        allowed_second, allowed_confirmation);
  require(fixture.volume.created_hidden_bytes == 48ULL * 1024ULL * 1024ULL,
          "the three-quarter boundary refused the size it must allow");

  // Nothing caps how many of them exist: a second name in the same directory
  // goes through the same unconditional flow.
  auto again_second = bytes("second-password-value");
  auto again_confirmation = bytes("second-password-value");
  fixture.controller.create_container_b(L"spare.DAT", directory, 40, 20,
                                        again_second, again_confirmation);
  require(fixture.volume.create_b_calls == 2 &&
              fixture.directory.holds_protected(fixture.volume.created_b_path) &&
              fixture.volume.created_b_display_name == L"spare" &&
              fixture.controller.container_statuses().size() == 2,
          "the second hidden-volume container was not adopted separately");
}

void test_capacity_rounds_the_attached_volume_to_whole_mib() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);

  // A real volume is never an exact MiB multiple: the header group alone costs
  // 128 KiB. The page shows the nearest MiB rather than refusing the value and
  // leaving the field empty.
  fixture.volume.containers[0].size_bytes =
      64ULL * 1024ULL * 1024ULL - 131072ULL;
  require(fixture.controller.container_statuses()[0].size_mib == 64,
          "an inexact volume capacity was not rounded for display");

  // Past the creation range it is still reported as unavailable.
  fixture.volume.containers[0].size_bytes =
      (static_cast<std::uint64_t>(kContainerCreationMaximumMib) + 2ULL) *
      1024ULL * 1024ULL;
  require(fixture.controller.container_statuses()[0].size_mib == 0,
          "a capacity outside the creation range was displayed anyway");
}

void test_card_reset_releases_the_session_and_refuses_while_mounted() {
  Fixture failed;
  failed.authentication.establish(root_secret());
  failed.entry.reject_reset = true;
  require_throws<std::runtime_error>([&] { failed.controller.reset_card_record(); },
      "an uncertain reset was reported successful");
  require(!failed.authentication.authenticated() && failed.volume.release_secrets_calls == 1 &&
      failed.volume.clear_operation_history_calls >= 1,
      "an uncertain reset retained the old session identity");
  Fixture unauthenticated;
  require(!unauthenticated.controller.can_reset_card_record(),
          "an unauthenticated session offered the reset");
  require_throws<std::system_error>(
      [&] { unauthenticated.controller.reset_card_record(); },
      "an unauthenticated session reset the card");
  require(unauthenticated.entry.reset_calls == 0,
          "the card was written without authentication");

  Fixture mounted;
  mounted.authentication.establish(root_secret());
  mounted.controller.create_container(L"work", mounted.directory.wide(), 64);
  mounted.volume.reject_unmount = true;

  // A mount that outlives its key material would hold a drive letter nothing
  // can release, so the card must not be written at all.
  require_throws<std::runtime_error>(
      [&] { mounted.controller.reset_card_record(); },
      "a still-mounted session reset the card");
  require(mounted.entry.reset_calls == 0,
          "the card was written while a volume was still attached");

  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);
  require(fixture.controller.can_reset_card_record(),
          "an authenticated session did not offer the reset");

  fixture.controller.reset_card_record();

  require(fixture.entry.reset_calls == 1, "the card record was not reset");
  require(!fixture.volume.mounted(),
          "the reset left a volume attached");
  // Nothing derived from the destroyed identity may outlive it.
  require(fixture.volume.release_secrets_calls == 1 &&
              fixture.volume.clear_operation_history_calls >= 1 &&
              !fixture.authentication.authenticated(),
          "the reset left session material behind");
  require(fixture.controller.state() != CardProductUiState::authenticated,
          "the reset left the page on an authenticated route");

  // A route that is no longer authenticated cannot reset again.
  require_throws<std::system_error>(
      [&] { fixture.controller.reset_card_record(); },
      "a released session reset the card a second time");
}

void test_inconsistent_container_status_fails_closed() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);
  const std::uint32_t id = fixture.controller.container_statuses()[0].id;
  fixture.controller.unmount_container(id);

  fixture.volume.inconsistent_unmounted_drive = L'D';
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.container_statuses()); },
      "UI displayed a drive letter for an unmounted container");
}

void test_unreadable_volume_is_shown_rather_than_rejected() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.controller.create_container(L"work", fixture.directory.wide(), 64);

  // The session could not read this volume: the row stays mounted, keeps its
  // letter and carries no capacity.
  fixture.volume.zero_mounted_size = true;
  fixture.volume.flag_unavailable = true;
  const auto statuses = fixture.controller.container_statuses();
  require(statuses.size() == 1 && statuses[0].mounted &&
              statuses[0].volume_unavailable && statuses[0].size_mib == 0 &&
              statuses[0].drive_letter != wchar_t{},
          "an unreadable volume was not shown as its own row");

  // Without the flag a mounted row without capacity is still inconsistent,
  // and the flag never travels with a capacity.
  fixture.volume.flag_unavailable = false;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.container_statuses()); },
      "a mounted row without capacity was accepted without the flag");
  fixture.volume.zero_mounted_size = false;
  fixture.volume.flag_unavailable = true;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.container_statuses()); },
      "an unavailable flag was accepted together with a capacity");
}

void test_operation_history_is_authenticated_sanitized_and_clearable() {
  Fixture fixture;
  require(fixture.controller.operation_history().empty() &&
              fixture.volume.operation_history_calls == 0,
          "an unauthenticated page queried operation history");
  require_throws<std::system_error>(
      [&] { fixture.controller.clear_operation_history(); },
      "an unauthenticated page cleared operation history");

  fixture.authentication.establish(root_secret());
  fixture.volume.operation_events = {
      {.sequence = 1,
       .completed_at_unix_ms = 1788253200000,
       .kind = usb_vault::application::OperationKind::session_opened,
       .outcome = usb_vault::application::OperationOutcome::succeeded},
      {.sequence = 2,
       .completed_at_unix_ms = 1788253201000,
       .kind = usb_vault::application::OperationKind::container_mounted,
       .outcome = usb_vault::application::OperationOutcome::succeeded,
       .container_name = L"work.hc",
       .drive_letter = L'X'},
  };
  const auto events = fixture.controller.operation_history();
  require(events.size() == 2 &&
              events[0].kind == OperationUiKind::session_opened &&
              events[0].outcome == OperationUiOutcome::succeeded &&
              events[0].container_name.empty() &&
              events[1].kind == OperationUiKind::container_mounted &&
              events[1].container_name == L"work.hc" &&
              events[1].drive_letter == L'X',
          "the controller changed a valid operation history projection");

  fixture.controller.clear_operation_history();
  require(fixture.volume.operation_events.empty() &&
              fixture.volume.clear_operation_history_calls == 1,
          "clear operation history did not reach the native session once");
}

void test_operation_history_rejects_paths_and_impossible_shapes() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.operation_events = {{
      .sequence = 1,
      .completed_at_unix_ms = 1788253200000,
      .kind = usb_vault::application::OperationKind::container_created,
      .outcome = usb_vault::application::OperationOutcome::succeeded,
      .container_name = L"D:\\Vault\\work.hc",
  }};
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.operation_history()); },
      "operation history exposed a full container path");

  fixture.volume.operation_events[0] = {
      .sequence = 2,
      .completed_at_unix_ms = 1788253201000,
      .kind = usb_vault::application::OperationKind::container_mounted,
      .outcome = usb_vault::application::OperationOutcome::failed,
      .container_name = L"work.hc",
      .drive_letter = L'X',
  };
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.operation_history()); },
      "a failed mount invented a confirmed drive in UI history");

  fixture.volume.operation_events[0] = {
      .sequence = 3,
      .completed_at_unix_ms = 1788253202000,
      .kind = usb_vault::application::OperationKind::session_opened,
      .outcome = usb_vault::application::OperationOutcome::failed,
  };
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.operation_history()); },
      "a failed authentication attempt entered operation history");

  fixture.volume.operation_events.assign(
      usb_vault::application::kOperationHistoryCapacity + 1, {});
  for (std::size_t index = 0;
       index < fixture.volume.operation_events.size(); ++index) {
    fixture.volume.operation_events[index] = {
        .sequence = index + 1,
        .completed_at_unix_ms =
            1788253200000 + static_cast<std::int64_t>(index),
        .kind = usb_vault::application::OperationKind::container_created,
        .outcome = usb_vault::application::OperationOutcome::succeeded,
        .container_name = L"work.hc",
    };
  }
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.operation_history()); },
      "an oversized operation history crossed the UI boundary");
}

void test_a_directory_listing_is_projected_only_when_authenticated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  require_throws<std::system_error>(
      [&] {
        static_cast<void>(fixture.controller.list_container_directory(7, L""));
      },
      "an unauthenticated route browsed a container");
  require(fixture.volume.file_manager_calls() == 0,
          "a refused browse still reached the session");

  fixture.authentication.establish(root_secret());
  fixture.volume.listing.truncated = true;
  fixture.volume.listing.entries.push_back({.name = L"notes.txt",
                                            .is_directory = false,
                                            .is_reparse_point = false,
                                            .size_bytes = 1234,
                                            .modified_unix_ms = 1700000000000LL});
  fixture.volume.listing.entries.push_back({.name = L"elsewhere",
                                            .is_directory = true,
                                            .is_reparse_point = true,
                                            .size_bytes = 0,
                                            .modified_unix_ms = 1700000000001LL});

  const DirectoryUiListing listing =
      fixture.controller.list_container_directory(7, L"reports");
  require(listing.truncated && listing.entries.size() == 2,
          "the listing lost its entries or its truncation flag");
  require(listing.entries[0].name == L"notes.txt" &&
              !listing.entries[0].is_directory && !listing.entries[0].is_link &&
              listing.entries[0].size_bytes == 1234 &&
              listing.entries[0].modified_unix_ms == 1700000000000LL,
          "a file entry was not projected as it stands");
  // A link is marked rather than hidden: a manager that silently omits what
  // Explorer shows is worse than one that says what is there.
  require(listing.entries[1].is_directory && listing.entries[1].is_link,
          "a link inside the container was not marked as one");
  require(fixture.volume.listed.size() == 1 &&
              fixture.volume.listed[0].id == 7 &&
              fixture.volume.listed[0].directory == L"reports",
          "the container and directory did not reach the session unchanged");
}

// A name no Windows file system can produce is evidence of a tampered volume,
// and the packed wire format below cannot carry one either.
void test_a_listing_entry_the_page_cannot_be_shown_fails_closed() {
  const std::wstring impossible_names[] = {L"", L"has\\a\\separator",
                                           L"has:a:colon",
                                           std::wstring(L"has\x01control")};
  for (const std::wstring& name : impossible_names) {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing.entries.push_back({.name = name});
    require_throws<std::runtime_error>(
        [&] {
          static_cast<void>(fixture.controller.list_container_directory(1, L""));
        },
        "a listing entry the page cannot be shown was projected anyway");
  }

  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.listing.entries.push_back(
      {.name = L"reports", .is_directory = true, .size_bytes = 4096});
  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(fixture.controller.list_container_directory(1, L""));
      },
      "a directory carrying a size was projected as one");
}

void test_file_manager_actions_are_refused_off_the_authenticated_route() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;

  require_throws<std::system_error>(
      [&] { fixture.controller.delete_container_entry(1, L"", L"a.txt"); },
      "an unauthenticated route deleted an entry");
  require_throws<std::system_error>(
      [&] {
        fixture.controller.rename_container_entry(1, L"", L"a.txt", L"b.txt");
      },
      "an unauthenticated route renamed an entry");
  require_throws<std::system_error>(
      [&] { fixture.controller.create_container_directory(1, L"", L"new"); },
      "an unauthenticated route created a directory");
  require_throws<std::system_error>(
      [&] {
        fixture.controller.move_container_entry(1, L"", L"a.txt", 2, L"");
      },
      "an unauthenticated route moved an entry");
  require_throws<std::system_error>(
      [&] {
        fixture.controller.copy_container_entry(1, L"", L"a.txt", 2, L"");
      },
      "an unauthenticated route copied an entry");
  require_throws<std::system_error>(
      [&] {
        fixture.controller.import_external_entry(1, L"", L"C:\\a\\b.txt", {});
      },
      "an unauthenticated route took a shell drop");

  require(fixture.volume.file_manager_calls() == 0,
          "a refused action still reached the session");
}

void test_file_manager_actions_reach_the_session_unchanged() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());

  fixture.controller.delete_container_entry(3, L"reports", L"old.txt");
  require(fixture.volume.deleted.size() == 1 &&
              fixture.volume.deleted[0].id == 3 &&
              fixture.volume.deleted[0].directory == L"reports" &&
              fixture.volume.deleted[0].name == L"old.txt",
          "the delete did not reach the session as it was asked");

  fixture.controller.rename_container_entry(3, L"reports", L"old.txt",
                                            L"new.txt");
  require(fixture.volume.renamed.size() == 1 &&
              fixture.volume.renamed[0].name == L"old.txt" &&
              fixture.volume.renamed[0].new_name == L"new.txt",
          "the rename did not carry both names");

  fixture.controller.create_container_directory(3, L"", L"archive");
  require(fixture.volume.created_directories.size() == 1 &&
              fixture.volume.created_directories[0].name == L"archive",
          "the new directory did not reach the session");

  fixture.controller.move_container_entry(3, L"reports", L"q1.txt", 4,
                                          L"archive");
  require(fixture.volume.moved.size() == 1 &&
              fixture.volume.moved[0].source_id == 3 &&
              fixture.volume.moved[0].source_directory == L"reports" &&
              fixture.volume.moved[0].name == L"q1.txt" &&
              fixture.volume.moved[0].destination_id == 4 &&
              fixture.volume.moved[0].destination_directory == L"archive",
          "the move did not carry both ends");

  fixture.controller.copy_container_entry(4, L"archive", L"q1.txt", 3, L"");
  require(fixture.volume.copied.size() == 1 &&
              fixture.volume.copied[0].source_id == 4 &&
              fixture.volume.copied[0].destination_id == 3 &&
              fixture.volume.copied[0].destination_directory.empty(),
          "the copy did not carry both ends");

  // A shell drop is the one file-manager action whose source is an ordinary
  // OS path. The controller passes it through untouched, together with the
  // cancellation the native drop target keeps asking.
  bool stop = true;
  fixture.controller.import_external_entry(
      3, L"photos", L"C:\\Users\\pictures\\2011.jpg",
      [&stop] { return stop; });
  require(fixture.volume.imported.size() == 1 &&
              fixture.volume.imported[0].id == 3 &&
              fixture.volume.imported[0].directory == L"photos" &&
              fixture.volume.imported[0].source ==
                  L"C:\\Users\\pictures\\2011.jpg" &&
              fixture.volume.imported[0].cancellation_asked,
          "the shell drop did not reach the session as it was handed over");
}

void test_exports_are_refused_off_the_authenticated_route() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;

  require_throws<std::system_error>(
      [&] {
        fixture.controller.export_container_entry(1, L"", L"q1.txt",
                                                  L"D:\\Out");
      },
      "an unauthenticated route exported an entry in plaintext");
  require_throws<std::system_error>(
      [&] { fixture.controller.export_container_file(1, L"D:\\Out"); },
      "an unauthenticated route migrated a container");
  require(fixture.volume.exported_entries.empty() &&
              fixture.volume.exported_files.empty(),
          "a refused export still reached the session");
}

// P5.9: the export password is confirmed and held to the offline policy
// before the session is reached, and both page buffers are wiped either way.
void test_encrypted_export_confirms_the_password_first() {
  Fixture fixture;
  const auto attempt = [&](const std::string_view first, const std::string_view second) {
    std::vector<std::byte> password = bytes(first);
    std::vector<std::byte> confirmation = bytes(second);
    try {
      fixture.controller.export_container_entry_encrypted(
          3, L"reports", L"q1.txt", L"D:\\Out", L"pkg", password, confirmation, {}, {});
    } catch (...) {
      require(std::ranges::all_of(password, [](std::byte b) { return b == std::byte{0}; }) &&
                  std::ranges::all_of(confirmation, [](std::byte b) { return b == std::byte{0}; }),
              "a refused export password was not wiped");
      throw;
    }
    require(std::ranges::all_of(password, [](std::byte b) { return b == std::byte{0}; }) &&
                std::ranges::all_of(confirmation, [](std::byte b) { return b == std::byte{0}; }),
            "an accepted export password was not wiped");
  };

  require_throws<std::system_error>(
      [&] { attempt("long enough export pass", "long enough export pass"); },
      "an unauthenticated route started an encrypted export");
  fixture.authentication.establish(root_secret());
  require_throws<usb_vault::security::PasswordConfirmationRejected>(
      [&] { attempt("long enough export pass", "long enough export pazz"); },
      "mismatched export passwords were accepted");
  require_throws<usb_vault::security::PasswordConfirmationRejected>(
      [&] { attempt("short", "short"); },
      "a weak export password was accepted");
  require(fixture.volume.encrypted_exports.empty(),
          "a refused export password still reached the session");

  attempt("long enough export pass", "long enough export pass");
  const std::string_view expected = "long enough export pass";
  require(fixture.volume.encrypted_exports.size() == 1 &&
              fixture.volume.encrypted_exports[0].id == 3 &&
              fixture.volume.encrypted_exports[0].directory == L"reports" &&
              fixture.volume.encrypted_exports[0].name == L"q1.txt" &&
              fixture.volume.encrypted_exports[0].destination == L"D:\\Out|pkg" &&
              fixture.volume.encrypted_password.size() == expected.size() &&
              std::equal(expected.begin(), expected.end(),
                         reinterpret_cast<const char*>(fixture.volume.encrypted_password.data())),
          "the encrypted export did not reach the session with its confirmed password");
}

// P5.9: the picked package path stays in the controller and reaches only the
// session; the page sees its file name. The path supports same-login retries.
void test_an_import_uses_the_picked_package_once() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::filesystem::path picked = std::filesystem::path(fixture.directory.wide()) / L"family photos";
  { std::ofstream(picked, std::ios::binary) << std::string(1000, 'x'); }
  fixture.chooser.existing_path = picked.wstring();
  require(fixture.controller.choose_import_file().name == L"family photos",
          "the page was not shown the picked file name");

  std::vector<std::byte> password = bytes("long enough export pass");
  const std::wstring taken = fixture.controller.selected_import_package();
  require(taken == picked.wstring() && fixture.controller.selected_import_package() == taken,
          "a pick was not retained for same-login retries");
  require(fixture.controller.import_package_into_container(5, L"in", taken, password, {}, {}) == L"Docs",
          "the import did not return the restored name");
  require(std::ranges::all_of(password, [](std::byte b) { return b == std::byte{0}; }),
          "the import password was not wiped");
  require(fixture.volume.imports.size() == 1 && fixture.volume.imports[0].id == 5 &&
              fixture.volume.imports[0].directory == L"in" &&
              fixture.volume.imports[0].destination == picked.wstring(),
          "the import did not carry the picked package to the session");

  std::vector<std::byte> again = bytes("long enough export pass");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(fixture.controller.import_package_into_container(5, L"in", L"", again, {}, {})); },
      "an import without a package was accepted");
  require(std::ranges::all_of(again, [](std::byte b) { return b == std::byte{0}; }),
          "a refused import password was not wiped");

  // A pick belongs to the login it was made under.
  require(fixture.controller.choose_import_file().name == L"family photos", "the second pick failed");
  fixture.authentication.clear_authentication();
  fixture.authentication.establish(root_secret());
  require(fixture.controller.selected_import_package().empty(),
          "a pick made before a new login was handed over after it");

  fixture.chooser.existing_path.clear();
  require(fixture.controller.choose_import_file().kind ==
              usb_vault::ui::CardProductUiSessionController::ImportFileKind::cancelled,
          "a cancelled pick named a file");
  require(fixture.volume.imports.size() == 1, "a refused import reached the session");
}

[[nodiscard]] std::string usb_vault_test_read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

void test_naming_a_selected_import_never_touches_the_package_file() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::filesystem::path root(fixture.directory.wide());
  const auto source = root / L"original package";
  const auto existing = root / L"already here";
  { std::ofstream(source, std::ios::binary) << "ciphertext stays identical";
    std::ofstream(existing, std::ios::binary) << "keep this file"; }
  fixture.chooser.existing_path = source.wstring();
  static_cast<void>(fixture.controller.choose_import_file());
  for (const auto name : {L"", L"..\\escape", L"CON", L"stream:name", L"trailing."}) {
    require_throws<std::invalid_argument>([&] { fixture.controller.set_selected_import_name(name); },
                                         "an import name a volume cannot hold was accepted");
  }
  // A name that clashes with a file beside the package is fine: nothing
  // there is renamed.
  fixture.controller.set_selected_import_name(L"already here");
  fixture.controller.set_selected_import_name(L"new name");
  require(fixture.controller.selected_import_package() == source.wstring() &&
              std::filesystem::exists(source) && !std::filesystem::exists(root / L"new name"),
          "naming the import renamed or moved the package file");
  std::ifstream content(source, std::ios::binary);
  require(std::string(std::istreambuf_iterator<char>(content), {}) == "ciphertext stays identical",
          "naming the import changed the package content");
  content.close();
  require(usb_vault_test_read(existing) == "keep this file", "a file beside the package was touched");
  fixture.authentication.clear_authentication();
  fixture.authentication.establish(root_secret());
  require_throws<std::logic_error>([&] { fixture.controller.set_selected_import_name(L"later login"); },
                                  "another login named a stale selection");
}

void test_a_renamed_package_is_verified_again_before_any_import_write() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::filesystem::path root(fixture.directory.wide());
  const auto source = root / L"note.txt";
  const auto package = root / L"original package";
  { std::ofstream(source, std::ios::binary) << "restore this"; }
  auto raw_password = bytes("long enough export pass");
  SecureBuffer password(raw_password.size());
  std::ranges::copy(raw_password, password.bytes().begin());
  usb_vault::backup::export_package_to_file_v1(source.wstring(), package.wstring(), password);
  fixture.chooser.existing_path = package.wstring();
  require(fixture.controller.choose_import_file().kind ==
              usb_vault::ui::CardProductUiSessionController::ImportFileKind::package,
          "an export package was not picked as a package");
  fixture.controller.set_selected_import_name(L"restored note.txt");
  const auto selected = fixture.controller.selected_import_package();
  auto wrong = bytes("incorrect password");
  require_throws<usb_vault::backup::ExportPackageAuthenticationRejected>([&] {
    static_cast<void>(fixture.controller.import_package_into_container(5, L"", selected, wrong, {}, {}));
  }, "renaming bypassed package authentication");
  require(fixture.volume.import_events.empty(), "a rejected renamed package reached the write pass");
  require_wiped(wrong, "a rejected renamed-package password was retained");
  static_cast<void>(fixture.controller.import_package_into_container(5, L"", selected, raw_password, {}, {}));
  require(fixture.volume.import_events == std::vector<std::wstring>{L"write:5:"},
          "a renamed package did not use the verified import path");
  require_wiped(raw_password, "a renamed-package password survived the import");
}

// P5.9: an import into a new container verifies first, creates through the
// ordinary creation path, and keeps the records only when both succeed.
void test_an_import_into_a_new_container_is_all_or_nothing() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::filesystem::path source = std::filesystem::path(fixture.directory.wide()) / L"note.txt";
  { std::ofstream(source, std::ios::binary) << "restored content"; }
  const std::filesystem::path package = std::filesystem::path(fixture.directory.wide()) / L"note package";
  {
    const std::string_view text = "long enough export pass";
    SecureBuffer password(text.size());
    std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())), password.bytes().begin());
    usb_vault::backup::export_package_to_file_v1(source.wstring(), package.wstring(), password);
  }
  const auto verify = [&] {
    std::vector<std::byte> password = bytes("long enough export pass");
    fixture.controller.verify_import(package.wstring(), password, {}, {});
    require(std::ranges::all_of(password, [](std::byte b) { return b == std::byte{0}; }),
            "the verification password was not wiped");
  };

  require(fixture.controller.verified_import_minimum_mib() == 0, "a minimum was given before verifying");
  verify();
  const std::uint32_t minimum = fixture.controller.verified_import_minimum_mib();
  require(minimum >= 1, "a verified package gave no minimum size");
  auto verified = fixture.controller.take_verified_import();
  require(verified != nullptr && fixture.controller.take_verified_import() == nullptr,
          "a verified package was not handed over exactly once");

  const std::uint32_t id = fixture.controller.import_into_new_container(
      std::move(verified), L"restored", fixture.directory.wide(), std::max<std::uint32_t>(minimum, 64), {}, {});
  require(fixture.volume.import_events ==
              std::vector<std::wstring>{L"defer", L"write:" + std::to_wstring(id) + L":", L"settle:keep"},
          "a successful import into a new container did not keep its records");

  fixture.volume.import_events.clear();
  verify();
  fixture.volume.fail_import_write = true;
  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(fixture.controller.import_into_new_container(
            fixture.controller.take_verified_import(), L"failed", fixture.directory.wide(),
            std::max<std::uint32_t>(minimum, 64), {}, {}));
      },
      "a failed import into a new container did not report the failure");
  require(fixture.volume.import_events.size() == 4 && fixture.volume.import_events[3] == L"settle:drop",
          "a failed import did not drop its records");
  const std::vector<std::wstring> failed_events = fixture.volume.import_events;
  fixture.volume.import_events.clear();
  verify();
  fixture.volume.fail_discard = true;
  require_throws<usb_vault::ui::CardProductUiSessionController::ImportRollbackIncomplete>(
      [&] {
        static_cast<void>(fixture.controller.import_into_new_container(
            fixture.controller.take_verified_import(), L"stuck", fixture.directory.wide(),
            std::max<std::uint32_t>(minimum, 64), {}, {}));
      },
      "an incomplete rollback was not reported as such");
  fixture.volume.fail_discard = false;
  fixture.volume.fail_import_write = false;
  fixture.volume.import_events = failed_events;
  require(fixture.volume.import_events.size() == 4 && fixture.volume.import_events[0] == L"defer" &&
              fixture.volume.import_events[2].starts_with(L"discard:") &&
              fixture.volume.import_events[3] == L"settle:drop",
          "a failed import did not discard its new container and drop its records");

  fixture.volume.import_events.clear();
  verify();
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(fixture.controller.import_into_new_container(
            fixture.controller.take_verified_import(), L"small", fixture.directory.wide(), 1, {}, {}));
      },
      "a container smaller than the package was accepted");
  require(fixture.volume.import_events.empty(), "a refused size still started an import");

  // A verified package belongs to the login it was verified under.
  verify();
  fixture.authentication.clear_authentication();
  fixture.authentication.establish(root_secret());
  require(fixture.controller.verified_import_minimum_mib() == 0 &&
              fixture.controller.take_verified_import() == nullptr,
          "a package verified before a new login was handed over after it");

  // A session change releases what an import held, without waiting for use.
  verify();
  fixture.authentication.clear_authentication();
  fixture.controller.release_stale_import_state();
  fixture.authentication.establish(root_secret());
  require(fixture.controller.take_verified_import() == nullptr,
          "a verified package survived the end of its login");
}

// P5.9: the generic import button tells containers and packages apart by
// size alone: a file container is whole sectors, a package is almost never.
void test_the_generic_import_sorts_containers_from_packages() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  using Kind = usb_vault::ui::CardProductUiSessionController::ImportFileKind;
  const std::filesystem::path root(fixture.directory.wide());
  const auto make = [&](const wchar_t* const name, const std::size_t size) {
    const std::filesystem::path path = root / name;
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
    return path.wstring();
  };

  fixture.chooser.existing_path = make(L"photos backup", 1000);
  auto choice = fixture.controller.choose_import_file();
  require(choice.kind == Kind::package && choice.name == L"photos backup",
          "a file that is not whole sectors was not taken as a package");
  require(fixture.controller.selected_import_package() == fixture.chooser.existing_path,
          "the package pick was not held for an import");
  require(fixture.volume.adopt_calls == 0 && fixture.volume.mount_calls == 0,
          "a package was treated as a container");

  fixture.chooser.existing_path = make(L"vault.HC", 2048);
  choice = fixture.controller.choose_import_file();
  require(choice.kind == Kind::container && fixture.volume.adopted_path == fixture.chooser.existing_path,
          "a sector-sized container file was not adopted as before");
  const auto shown = [&](const std::wstring& path) {
    for (const auto& entry : fixture.volume.containers) {
      if (entry.path == path) return entry.name;
    }
    return std::wstring{};
  };
  require(shown(fixture.chooser.existing_path) == L"本地磁盘",
          "an imported container was not named 本地磁盘");
  const std::wstring first_import = fixture.chooser.existing_path;

  // The name this product gives new containers: a braced GUID, no suffix.
  fixture.chooser.existing_path = make(L"{37218019-3B14-40C2-BBCA-17C3FF88C262}", 2048);
  choice = fixture.controller.choose_import_file();
  require(choice.kind == Kind::container && fixture.volume.adopted_path == fixture.chooser.existing_path,
          "a container named the product's way was not adopted directly");
  require(shown(fixture.chooser.existing_path) == L"本地磁盘 (2)",
          "a second imported container was not numbered like other clashing names");
  // Importing a listed container again keeps its name.
  fixture.chooser.existing_path = first_import;
  static_cast<void>(fixture.controller.choose_import_file());
  require(shown(first_import) == L"本地磁盘", "a listed container was renamed on a repeated import");
  fixture.chooser.existing_path = make(L"{37218019-3B14-40C2-BBCA-17C3FF88C26}", 2048);
  require(fixture.controller.choose_import_file().kind == Kind::undecided,
          "a name that is almost a braced GUID was taken as a container");

  fixture.chooser.existing_path = make(L"unnamed", 4096);
  choice = fixture.controller.choose_import_file();
  require(choice.kind == Kind::undecided && choice.name == L"unnamed",
          "a sector-sized file without a container extension was decided for the user");
  require(fixture.controller.treat_undecided_import_as_package() == L"unnamed" &&
              fixture.controller.selected_import_package() == fixture.chooser.existing_path,
          "an undecided file could not be imported as a package");
  require(!fixture.controller.adopt_undecided_import_file(), "a settled pick was settled twice");

  choice = fixture.controller.choose_import_file();
  require(choice.kind == Kind::undecided && fixture.controller.adopt_undecided_import_file() &&
              fixture.volume.adopted_path == fixture.chooser.existing_path,
          "an undecided file could not be imported as a container");

  choice = fixture.controller.choose_import_file();
  fixture.authentication.clear_authentication();
  fixture.authentication.establish(root_secret());
  require(fixture.controller.treat_undecided_import_as_package().empty(),
          "an undecided pick outlived its login");

  fixture.chooser.existing_path.clear();
  require(fixture.controller.choose_import_file().kind == Kind::cancelled, "a cancelled pick named a file");
}

void test_exports_reach_the_session_unchanged() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());

  fixture.controller.export_container_entry(3, L"reports", L"q1.txt",
                                            L"D:\\Out");
  require(fixture.volume.exported_entries.size() == 1 &&
              fixture.volume.exported_entries[0].id == 3 &&
              fixture.volume.exported_entries[0].directory == L"reports" &&
              fixture.volume.exported_entries[0].name == L"q1.txt" &&
              fixture.volume.exported_entries[0].destination == L"D:\\Out",
          "the plaintext export did not carry all four of its parts");

  fixture.controller.export_container_folder(4, L"D:\\Migration", L"工作资料");
  require(fixture.volume.exported_files.size() == 1 &&
              fixture.volume.exported_files[0].id == 4 &&
              fixture.volume.exported_files[0].name == L"工作资料" &&
              fixture.volume.exported_files[0].destination == L"D:\\Migration",
          "the migration export did not carry the container, folder name and location");

  // The controller decides nothing about either: the location is checked by
  // the session, which is also the only layer that knows whether it lands
  // inside a container or whether that container is mounted.
  require(fixture.volume.exported_entries.size() == 1 &&
              fixture.volume.exported_files.size() == 1,
          "the controller turned one export into more than one call");
}

void test_login_reset_mount_choices() {
  for (int scenario = 0; scenario < 6; ++scenario) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    fixture.volume.mounted_state = true;
    fixture.reset_mounts.values.push_back({.drive_letter = L'X', .unique_id = 1});
    if (scenario == 1) fixture.reset_mounts.values.clear();
    if (scenario == 2) fixture.reset_mounts.query_fails = true;
    require(fixture.controller.can_login_reset(), "mount state blocked reset");
    const auto prepared = fixture.controller.prepare_login_reset();
    require(prepared.mounts_known == (scenario != 2), "query failure invented mount facts");
    if (scenario == 3) {
      fixture.reset_mounts.values.push_back({.drive_letter = L'Y', .unique_id = 2});
      fixture.controller.force_login_reset_mounts(prepared.ticket);
      require(fixture.reset_mounts.forced == 1 && fixture.reset_mounts.values.size() == 1 &&
              fixture.reset_mounts.values[0].drive_letter == L'Y', "force expanded targets");
    }
    if (scenario == 4 || scenario == 5) {
      if (scenario == 4) fixture.reset_mounts.values[0].unique_id++;
      else fixture.reset_mounts.force_fails = true;
      require_throws<std::exception>([&] { fixture.controller.force_login_reset_mounts(prepared.ticket); }, "unsafe force accepted");
      require(fixture.controller.login_reset_pending(prepared.ticket), "force failure prevented skipping");
    }
    fixture.controller.confirm_login_reset(prepared.ticket);
    require(fixture.entry.login_reset_calls == 1 && fixture.volume.unmount_calls == 0,
            "reset blocked or implicitly unmounted");
    require(fixture.reset_mounts.forced == (scenario == 3 ? 1 : 0), "skip performed force");
    require(usb_vault::ui::prepare_ui_session_for_close(fixture.controller) && fixture.volume.unmount_calls == 0, "close unmounted skipped volumes");
  }
}

}  // namespace

int main() {
  try {
    test_login_reset_mount_choices();
    test_state_projection_is_complete_and_authentication_wins();
    test_activation_gates_card_actions_and_survives_expiry_in_session();
    test_login_normalizes_wipes_and_preserves_retry_semantics();
    test_post_authentication_failure_projects_authenticated_state();
    test_initialization_normalizes_both_inputs_and_creates_proof();
    test_initialization_immediately_supports_hidden_creation();
    test_failed_initialization_handoff_clears_authentication();
    test_initialization_rejections_wipe_both_before_entry_action();
    test_wrong_route_wipes_without_secret_processing();
    test_overlapping_initialization_inputs_are_rejected_and_wiped();
    test_residual_mount_blocks_new_card_actions_until_cleanup();
    test_unmount_forwards_only_unified_active_state();
    test_container_collection_is_projected_only_when_authenticated();
    test_a_location_without_room_is_refused_before_anything_is_written();
    test_container_creation_is_bounded_and_validated();
    test_browsing_for_a_directory_only_reports_the_choice();
    test_adopted_container_remounts_without_reselecting_its_file();
    test_partition_pair_controller();
    test_container_b_commits_the_card_before_the_volume();
    test_a_survey_passes_the_password_through_and_wipes_it();
    test_a_survey_wipes_the_password_when_it_fails();
    test_maintenance_is_refused_outside_an_authenticated_session();
    test_opening_maintenance_names_only_the_container();
    test_maintenance_capacity_is_projected_conservatively_and_can_end();
    test_container_b_rejects_unusable_requests();
    test_capacity_rounds_the_attached_volume_to_whole_mib();
    test_card_reset_releases_the_session_and_refuses_while_mounted();
    test_inconsistent_container_status_fails_closed();
    test_unreadable_volume_is_shown_rather_than_rejected();
    test_operation_history_is_authenticated_sanitized_and_clearable();
    test_operation_history_rejects_paths_and_impossible_shapes();
    test_dropping_a_container_takes_the_record_and_not_the_file();
    test_dropping_a_container_is_refused_off_the_authenticated_route();
    test_the_remember_switch_reads_anywhere_and_changes_only_inside();
    test_importing_only_mounts_when_the_switch_asks_for_it();
    test_the_import_switch_reads_anywhere_and_changes_only_inside();
    test_a_directory_listing_is_projected_only_when_authenticated();
    test_a_listing_entry_the_page_cannot_be_shown_fails_closed();
    test_file_manager_actions_are_refused_off_the_authenticated_route();
    test_file_manager_actions_reach_the_session_unchanged();
    test_exports_are_refused_off_the_authenticated_route();
    test_exports_reach_the_session_unchanged();
    test_encrypted_export_confirms_the_password_first();
    test_an_import_uses_the_picked_package_once();
    test_naming_a_selected_import_never_touches_the_package_file();
    test_a_renamed_package_is_verified_again_before_any_import_write();
    test_an_import_into_a_new_container_is_all_or_nothing();
    test_the_generic_import_sorts_containers_from_packages();
    std::cout << "card_product_ui_session_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_ui_session_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
