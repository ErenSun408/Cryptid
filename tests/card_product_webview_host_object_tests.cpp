#include "usb_vault/ui/card_product_webview_host_object.h"

#include "usb_vault/application/activation_store_v1.h"
#include "usb_vault/backup/export_package_file.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/contacts/contact_identity_v1.h"
#include "usb_vault/contacts/contact_store_v1.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "page_event_test_support.h"
#include "usb_vault/ui/card_configuration_webview_host_object.h"
#include "usb_vault/security/offline_card_password_policy.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "usb_vault/volume/volume_provider.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <span>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>
#include <chrono>

namespace {

using Microsoft::WRL::ComPtr;
using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ContainerAStatus;
using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::ConfirmedPassword;
using usb_vault::security::OfflineCardPasswordPolicy;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;
using usb_vault::ui::create_card_product_webview_host_object;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
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

[[nodiscard]] SecureBuffer root_secret() {
  SecureBuffer result(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
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
  usb_vault::application::CreationFeedback creation_feedback;
  std::atomic<bool> pause_creation{false};
  std::atomic<bool> creation_entered{false};
  std::exception_ptr creation_failure;
  bool skip_cancel_checkpoint{false};
  std::atomic<bool> pause_finalization{false};
  std::atomic<bool> finalization_entered{false};
  void set_creation_feedback(usb_vault::application::CreationFeedback feedback) override {
    creation_feedback = std::move(feedback);
  }
  usb_vault::application::CreationCancellation creation_cancelled;
  void set_creation_cancellation(
      usb_vault::application::CreationCancellation cancelled) override {
    creation_cancelled = std::move(cancelled);
  }
  bool logging_enabled{true};
  bool force_preference{};
  bool file_hiding{};
  bool container_file_hidden(std::uint32_t) override { return file_hiding; }
  bool hide_new_file_containers() override { return file_hiding; }
  void set_hide_new_file_containers(bool value) override {
    if (extension_save_fails) throw std::runtime_error("settings write failed");
    file_hiding = value;
  }
  std::uint32_t visibility_id{};
  void set_container_file_hidden(std::uint32_t id, bool hidden) override { visibility_id = id; file_hiding = hidden; }
  unsigned int extension_preference{0};
  bool extension_save_fails{};
  unsigned int container_extension_preference() override { return extension_preference; }
  void set_container_extension_preference(unsigned int value) override {
    if (extension_save_fails) throw std::runtime_error("settings write failed");
    extension_preference = value;
  }
  bool auto_preference{};
  bool auto_mount_enabled() override { return auto_preference; }
  void set_auto_mount_enabled(bool enabled) override { auto_preference = enabled; }
  bool force_unmount_enabled() override { return force_preference; }
  void set_force_unmount_enabled(bool enabled) override { force_preference = enabled; }
  std::vector<std::uint32_t> saved_order;
  std::uint32_t renamed_container_id{};
  std::wstring container_alias;
  bool fail_container_rename{};
  void rename_container(std::uint32_t id, std::wstring_view name) override {
    if (fail_container_rename) throw std::runtime_error("index write failed");
    renamed_container_id = id;
    container_alias = name;
  }
  void reorder_containers(std::span<const std::uint32_t> ids) override {
    if (ids.size() != containers.size()) throw std::invalid_argument("incomplete order");
    for (auto id : ids) {
      if (std::none_of(containers.begin(), containers.end(), [id](const auto& entry) { return entry.id == id; }))
        throw std::invalid_argument("unknown container");
    }
    saved_order.assign(ids.begin(), ids.end());
  }
  bool operation_logging_enabled() override { return logging_enabled; }
  void set_operation_logging_enabled(bool enabled) override { logging_enabled = enabled; }
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

  void await_creation() {
    if (creation_feedback) { creation_feedback(1, 100, 0); creation_feedback(2, 100, 0); creation_feedback(3, 41, 0); }
    creation_entered.store(true);
    for (int i = 0; pause_creation.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  void await_publication() {
    if (creation_failure) std::rethrow_exception(creation_failure);
    // What the real session does once the helper is done: a cancel still
    // pending here means the container is dropped rather than published.
    if (!skip_cancel_checkpoint && creation_cancelled && creation_cancelled()) {
      throw std::system_error(usb_vault::volume_creation::make_error_code(
          usb_vault::volume_creation::CreationError::operation_cancelled));
    }
    if (reject_create) throw std::runtime_error("creation failed");
    if (creation_feedback) creation_feedback(4, 0, 0);
    finalization_entered.store(true);
    for (int i = 0; pause_finalization.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  usb_vault::application::CreationCancellation creation_revoked;
  void set_creation_revocation(usb_vault::application::CreationCancellation callback) override { creation_revoked=std::move(callback); }
  bool begin_partition_conversion(const usb_vault::volume::PartitionCandidate& target,
      const std::function<bool(const usb_vault::volume::PartitionCandidate&)>& confirm) override {
    await_creation();
    require(creation_revoked && !creation_revoked(),"conversion missing revocation query");
    if(!confirm(target))throw std::logic_error("conversion consent expired");
    ++partition_creations; created_partition=target;
    return true;
  }
  bool resume_partition_conversion(std::uint32_t id) override {
    await_creation();
    require(id==42 && creation_revoked,"resume binding missing");
    if(creation_revoked())throw std::system_error(usb_vault::volume_creation::make_error_code(usb_vault::volume_creation::CreationError::operation_cancelled));
    return creation_cancelled && creation_cancelled();
  }
  int partition_creations{};
  usb_vault::volume::PartitionCandidate created_partition;
  std::uint32_t create_partition_pair(const usb_vault::volume::PartitionCandidate& target,
      std::uint64_t hidden, const SecureBuffer& decoy,
      const std::function<bool(const usb_vault::volume::PartitionCandidate&)>& confirm) override {
    require(hidden==16ULL*1024*1024 && !decoy.empty(), "pair inputs missing");
    return create_partition(target, confirm);
  }
  std::uint32_t create_partition(const usb_vault::volume::PartitionCandidate& target,
      const std::function<bool(const usb_vault::volume::PartitionCandidate&)>& confirm) override {
    await_creation();
    auto wrong = target; ++wrong.length_bytes;
    require(!confirm(wrong), "confirmation accepted different target");
    if (!confirm(target)) throw std::runtime_error("partition consent expired");
    ++partition_creations;
    created_partition = target;
    await_publication();
    return 42;
  }

  std::uint32_t create_container(const std::wstring_view path,
                                 const std::uint64_t size_bytes) override {
    await_creation();
    ++create_calls;
    created_path.assign(path);
    created_size_bytes = size_bytes;
    await_publication();
    return adopt(path, size_bytes);
  }

  std::uint32_t create_container_b(
      const std::wstring_view path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer&) override {
    await_creation();
    ++create_b_calls;
    created_b_path.assign(path);
    created_outer_bytes = outer_size_bytes;
    created_hidden_bytes = hidden_size_bytes;
    await_publication();
    return adopt(path, outer_size_bytes);
  }

  SecureBuffer initialized_password;
  void complete_initialization(SecureBuffer password) override { initialized_password = std::move(password); }
  SecureBuffer copy_session_password() override {
    if (!initialized_password.empty()) {
      SecureBuffer copy(initialized_password.size());
      std::copy(initialized_password.bytes().begin(), initialized_password.bytes().end(), copy.bytes().begin());
      return copy;
    }
    const std::vector<std::byte> retained = bytes("login-password");
    SecureBuffer copy(retained.size());
    for (std::size_t index = 0; index < retained.size(); ++index) {
      copy.bytes()[index] = retained[index];
    }
    return copy;
  }

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
  std::wstring created_b_path;
  std::uint64_t created_outer_bytes{};
  std::uint64_t created_hidden_bytes{};
  int create_b_calls{};

  std::uint32_t mount_container(const std::wstring_view path) override {
    ++mount_calls;
    for (Adopted& entry : containers) {
      if (entry.path != path) continue;
      entry.mounted = true;
      mounted_state = true;
      return entry.id;
    }
    return adopt(path, 64ULL * 1024ULL * 1024ULL);
  }

  std::vector<std::uint32_t> decoy_maintenance_candidates() override { return survey_result; }

  usb_vault::application::DecoyMaintenanceSurvey survey_decoy_maintenance(
      const usb_vault::security::SecureBuffer& decoy_password_utf8,
      const usb_vault::application::SurveyProgress& progress,
      const usb_vault::application::SurveyCancellation& cancelled) override {
    await_volume_operation();
    survey_password.assign(decoy_password_utf8.bytes().begin(),
                           decoy_password_utf8.bytes().end());
    ++survey_calls;
    for (std::uint32_t done = 1; done <= survey_total; ++done) {
      if (cancelled && cancelled()) return {.cancelled = true};
      if (progress) progress(done, survey_total);
    }
    if (survey_fails) throw std::runtime_error("survey failed");
    return {.maintainable = survey_result, .unreachable = survey_unreachable};
  }

  wchar_t open_surveyed_maintenance(const std::uint32_t id) override {
    await_volume_operation();
    ++open_maintenance_calls;
    opened_maintenance_id = id;
    return L'Q';
  }

  usb_vault::application::DecoyMaintenanceSpace decoy_maintenance_space(
      const std::uint32_t id) override {
    await_volume_operation();
    ++maintenance_space_calls;
    maintenance_space_id = id;
    if (fail_maintenance_space) throw std::runtime_error("measure failed");
    return maintenance_space;
  }

  bool maintenance_blocked = false;
  bool decoy_maintenance_write_blocked(std::uint32_t id) override {
    if (id != 7) throw std::runtime_error("unknown maintenance volume");
    return maintenance_blocked;
  }

  void end_decoy_maintenance(const std::uint32_t id) override {
    await_volume_operation();
    ++end_maintenance_calls;
    ended_maintenance_id = id;
  }

  void mount_adopted_container(const std::uint32_t id) override {
    await_volume_operation();
    batch_calls.push_back(id);
    if (id == batch_fail_id) throw std::system_error(
        usb_vault::volume::make_error_code(usb_vault::volume::DriverError::volume_busy));
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

  int mount_adopted_calls{};

  void unmount_container(const std::uint32_t id) override {
    await_volume_operation();
    batch_calls.push_back(id);
    if (id == batch_fail_id) throw std::system_error(
        usb_vault::volume::make_error_code(usb_vault::volume::DriverError::volume_busy));
    ++unmount_container_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = false;
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
    // Each way a listing can fail, so the bridge can be asked to tell them
    // apart instead of answering every one of them the same way.
    if (listing_not_mounted) throw std::logic_error("that container is not mounted");
    if (listing_refused) throw std::runtime_error("the directory resolves outside its volume");
    if (listing_system_error) {
      throw std::system_error(static_cast<int>(listing_system_error),
                              std::system_category(), "open the directory");
    }
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

  [[nodiscard]] std::size_t file_manager_calls() const noexcept {
    return listed.size() + deleted.size() + renamed.size() +
           created_directories.size() + moved.size() + copied.size();
  }

  usb_vault::volume::VolumeDirectoryListing listing;
  bool reject_listing{};
  bool listing_not_mounted{};
  bool listing_refused{};
  unsigned long listing_system_error{};
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

  void export_container_selection(std::span<const usb_vault::application::ExportSelectionEntry> entries,
      std::wstring_view destination, std::wstring_view folder, bool encrypted,
      const usb_vault::security::SecureBuffer& password, const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
    require(encrypted == !password.empty(), "selection export password and mode must agree");
    selected_exports.assign(entries.begin(), entries.end());
    selected_folder = folder; selected_destination = destination;
    await_volume_operation();
    if (cancelled()) throw std::system_error(ERROR_CANCELLED, std::system_category());
    progress(40, 40);
  }
  std::vector<usb_vault::application::ExportSelectionEntry> selected_exports;
  std::wstring selected_folder, selected_destination;
  void export_container_selection_for_recipients(
      std::span<const usb_vault::application::ExportSelectionEntry>, std::wstring_view, std::wstring_view,
      const usb_vault::contacts::ContactIdentity&,
      const std::span<const usb_vault::contacts::ContactPublicKey> recipients,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
    if (cancelled()) throw std::system_error(ERROR_CANCELLED, std::system_category());
    progress(40, 40);
    recipient_exports.emplace_back(recipients.begin(), recipients.end());
  }
  std::vector<std::vector<usb_vault::contacts::ContactPublicKey>> recipient_exports;
  std::wstring write_verified_transfer_import(
      const std::uint32_t id, std::wstring_view, usb_vault::backup::ImportPackageV1&,
      const usb_vault::contacts::ContactIdentity&, const std::function<bool()>&,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) override {
    progress(2, 10, 10);
    transfer_imports.push_back(id);
    return L"Docs";
  }
  std::vector<std::uint32_t> transfer_imports;
  void export_container_entry_encrypted(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name,
      const std::wstring_view absolute_destination_directory,
      const std::wstring_view package_name,
      const usb_vault::security::SecureBuffer&,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) override {
    await_volume_operation();
    if (cancelled()) {
      throw std::system_error(ERROR_CANCELLED, std::system_category());
    }
    progress(0, 40);
    progress(40, 40);
    encrypted_exports.push_back(
        {.id = id,
         .directory = std::wstring(relative_directory),
         .name = std::wstring(name),
         .destination = std::wstring(absolute_destination_directory) + L"|" +
                        std::wstring(package_name)});
  }
  std::vector<ExportCall> encrypted_exports;

  std::wstring import_package_into_container(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view package,
      const usb_vault::security::SecureBuffer&,
      const std::function<bool()>& cancelled,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) override {
    progress(1, 10, 10);
    if (cancelled()) throw std::system_error(ERROR_CANCELLED, std::system_category());
    if (import_failure == 1) throw usb_vault::backup::ExportPackageAuthenticationRejected();
    if (import_failure == 10) throw usb_vault::backup::ExportPackageRejected();
    progress(2, 10, 10);
    imports.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .destination = std::wstring(package)});
    return L"Docs";
  }
  std::vector<ExportCall> imports;
  unsigned int import_failure{};

  std::vector<ExportCall> exported_entries;
  std::vector<ExportCall> exported_files;

  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    if (fail_container_statuses) {
      throw std::runtime_error("container status query failed");
    }
    std::vector<usb_vault::application::ContainerStatus> result;
    result.reserve(containers.size());
    for (const Adopted& entry : containers) {
      const bool unavailable = entry.mounted && entry.unavailable;
      result.push_back({.id = entry.id,
                        .name = entry.name,
                        .mounted = entry.mounted,
                        .size_bytes =
                            entry.mounted && !unavailable ? entry.size_bytes : 0,
                        .drive_letter =
                            entry.mounted ? entry.drive_letter : wchar_t{},
                        .volume_unavailable = unavailable});
    }
    return result;
  }

  struct Adopted final {
    std::uint32_t id{};
    std::wstring path;
    std::wstring name;
    std::uint64_t size_bytes{};
    wchar_t drive_letter{};
    bool mounted{};
    bool unavailable{};
  };

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

  std::vector<Adopted> containers;
  std::vector<std::uint32_t> batch_calls;
  std::vector<std::byte> survey_password;
  std::vector<std::uint32_t> survey_result;
  std::uint32_t survey_total{};
  std::uint32_t survey_unreachable{};
  unsigned int survey_calls{};
  unsigned int open_maintenance_calls{};
  std::uint32_t opened_maintenance_id{};
  usb_vault::application::DecoyMaintenanceSpace maintenance_space;
  unsigned int maintenance_space_calls{};
  unsigned int end_maintenance_calls{};
  std::uint32_t maintenance_space_id{};
  std::uint32_t ended_maintenance_id{};
  bool fail_maintenance_space{};
  bool survey_fails{};
  std::uint32_t batch_fail_id{};
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  bool reject_create{};
  bool fail_container_statuses{};
  bool fail_operation_history{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};
  int forget_container_calls{};

  [[nodiscard]] ContainerAStatus container_a_status() override {
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_size_bytes,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  [[nodiscard]] std::vector<usb_vault::application::OperationEvent>
  operation_history() override {
    if (fail_operation_history) {
      throw std::runtime_error("operation history query failed");
    }
    return operation_events;
  }

  void release_session_secrets() noexcept override {}

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
  }

  void unmount_active() override {
    await_volume_operation();
    ++unmount_calls;
    if (reject_unmount) throw std::runtime_error("unmount failed");
    container_a_mounted = false;
    mounted_state = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("forced unmount is outside the UI contract");
  }
  void force_unmount_container(std::uint32_t id) override {
    await_volume_operation();
    ++manual_force_calls;
    manual_force_id = id;
    manual_force_ids.push_back(id);
    if (id == batch_fail_id) throw std::system_error(
        usb_vault::volume::make_error_code(usb_vault::volume::DriverError::volume_busy));
    for (auto& entry : containers) if (entry.id == id) entry.mounted = false;
  }
  void force_unmount_user_active() override {
    await_volume_operation();
    ++manual_force_calls;
    manual_force_id = 0;
    if (retain_mount_after_force) return;
    for (auto& entry : containers) entry.mounted = false;
    mounted_state = false;
  }
  int manual_force_calls{};
  bool retain_mount_after_force{};
  std::vector<std::uint32_t> manual_force_ids;
  std::uint32_t manual_force_id{};

  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  CardAuthenticationSession& authentication_;
  std::vector<std::byte> login_bytes;
  std::vector<usb_vault::application::OperationEvent> operation_events;
  bool mounted_state{};
  bool reject_unmount{};
  std::atomic<bool> pause_volume{false};
  std::atomic<bool> volume_entered{false};
  std::thread::id volume_thread;
  void await_volume_operation() {
    volume_thread = std::this_thread::get_id();
    volume_entered.store(true);
    for (int i = 0; pause_volume.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (volume_failure) throw std::system_error(volume_failure, "private path and diagnostic must not cross UI bridge");
  }
  std::error_code volume_failure;
  bool reject_create_a{};
  bool container_a_exists{};
  bool container_a_mounted{};
  std::uint64_t container_a_size_bytes{};
  std::uint64_t created_a_size_bytes{};
  int consume_calls{};
  int create_a_calls{};
  int unmount_calls{};
  int clear_operation_history_calls{};
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
  }

  void reset_card_record(
      const usb_vault::application::IVolatileCardAuthenticationSession&
          authentication) override {
    if (!authentication.authenticated()) {
      throw std::runtime_error("card reset was not authenticated");
    }
    ++reset_calls;
  }

  std::atomic<bool> delay_reset_prepare{false}, reset_prepare_entered{false};
  std::uint32_t prepare_login_reset() override {
    reset_prepare_entered.store(true);
    for (int i = 0; delay_reset_prepare.load() && i < 1000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    reset_ticket = ++reset_serial; return reset_ticket;
  }
  int cancel_calls{};
  void cancel_login_reset() noexcept override { ++cancel_calls; reset_ticket = 0; }
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
  std::vector<std::byte> existing_password_bytes;
  std::vector<std::byte> second_password_bytes;
  int second_password_calls{};

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }
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
  int initialization_calls{};
  int login_calls{};
};

// Adoption stays native, and so does the browse button behind the form's
// save-location field: the page can only ask, so the fixture decides what
// comes back and an empty string stands for a cancelled dialog.
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

  [[nodiscard]] std::wstring choose_contact_key_file() override { return key_file_path; }
  [[nodiscard]] std::wstring choose_contact_key_save_path() override { return key_save_path; }

  std::wstring directory_path;
  std::wstring existing_path;
  std::wstring key_file_path;
  std::wstring key_save_path;
  int directory_calls{};
  int existing_calls{};
};

// The form names a location instead of picking one, and the boundary checks
// that the location exists, so the fixture needs a real directory.
class ScopedDirectory final {
 public:
  explicit ScopedDirectory(const wchar_t* const leaf)
      : path_(std::filesystem::temp_directory_path() /
              L"usb-vault-host-tests" / leaf) {
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

struct Fixture final {
  Fixture()
      : directory(std::to_wstring(++g_fixture_serial).c_str()),
        volume(authentication),
        controller(entry, authentication, volume, policy, chooser) {}

  FakeEntry entry;
  ScopedDirectory directory;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume;
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller;
};

void initialize_byte_array(VARIANTARG& value,
                           const std::span<const std::byte> input) {
  ::VariantInit(&value);
  SAFEARRAY* const array =
      ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(input.size()));
  if (array == nullptr) throw std::runtime_error("SAFEARRAY allocation failed");
  value.vt = VT_ARRAY | VT_UI1;
  value.parray = array;

  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(array, &raw)),
          "SAFEARRAY access failed");
  auto* const data = static_cast<std::byte*>(raw);
  for (std::size_t index = 0; index < input.size(); ++index) {
    data[index] = input[index];
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(array)),
          "SAFEARRAY release failed");
}

void require_array_wiped(const VARIANTARG& value,
                         const std::size_t size,
                         const char* const message) {
  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(value.parray, &raw)),
          "SAFEARRAY inspection failed");
  const auto* const data = static_cast<const std::byte*>(raw);
  for (std::size_t index = 0; index < size; ++index) {
    require(data[index] == std::byte{0}, message);
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(value.parray)),
          "SAFEARRAY inspection release failed");
}

class ByteArray final {
 public:
  explicit ByteArray(const std::span<const std::byte> input)
      : size_(input.size()) {
    initialize_byte_array(value_, input);
  }

  ~ByteArray() noexcept { static_cast<void>(::VariantClear(&value_)); }

  ByteArray(const ByteArray&) = delete;
  ByteArray& operator=(const ByteArray&) = delete;

  [[nodiscard]] VARIANTARG& value() noexcept { return value_; }
  void require_wiped(const char* const message) const {
    require_array_wiped(value_, size_, message);
  }

 private:
  VARIANTARG value_{};
  std::size_t size_{};
};

class InitializationArguments final {
 public:
  InitializationArguments(const std::span<const std::byte> password,
                          const std::span<const std::byte> confirmation)
      : password_size_(password.size()),
        confirmation_size_(confirmation.size()) {
    // IDispatch receives positional arguments in reverse order.
    initialize_byte_array(values_[0], confirmation);
    initialize_byte_array(values_[1], password);
  }

  ~InitializationArguments() noexcept {
    static_cast<void>(::VariantClear(&values_[0]));
    static_cast<void>(::VariantClear(&values_[1]));
  }

  InitializationArguments(const InitializationArguments&) = delete;
  InitializationArguments& operator=(const InitializationArguments&) = delete;

  [[nodiscard]] DISPPARAMS parameters() noexcept {
    return DISPPARAMS{values_.data(), nullptr, 2, 0};
  }

  void replace_password_with_bstr() {
    static_cast<void>(::VariantClear(&values_[1]));
    values_[1].vt = VT_BSTR;
    values_[1].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[1].bstrVal != nullptr, "test BSTR allocation failed");
    password_is_array_ = false;
  }

  void replace_confirmation_with_bstr() {
    static_cast<void>(::VariantClear(&values_[0]));
    values_[0].vt = VT_BSTR;
    values_[0].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[0].bstrVal != nullptr, "test BSTR allocation failed");
    confirmation_is_array_ = false;
  }

  void require_password_wiped() const {
    require(password_is_array_, "password is not an array");
    require_array_wiped(values_[1], password_size_,
                        "initialization password SAFEARRAY survived");
  }

  void require_confirmation_wiped() const {
    require(confirmation_is_array_, "confirmation is not an array");
    require_array_wiped(values_[0], confirmation_size_,
                        "initialization confirmation SAFEARRAY survived");
  }

 private:
  std::array<VARIANTARG, 2> values_{};
  std::size_t password_size_{};
  std::size_t confirmation_size_{};
  bool password_is_array_{true};
  bool confirmation_is_array_{true};
};

class RecordingCloseSink final : public usb_vault::ui::IUiCloseRequestSink {
 public:
  void request_close() noexcept override { ++close_calls; }
  bool close_to_tray() const noexcept override { return tray_enabled; }
  bool set_close_to_tray(bool value) noexcept override {
    if (!allow_tray_write) return false;
    tray_enabled = value; return true;
  }
  bool tray_enabled{}, allow_tray_write{true};

  bool request_configuration() noexcept override { ++configuration_calls; return allow_configuration; }
  bool request_login() noexcept override { ++login_calls; return allow_login; }
  bool request_card_backup() noexcept override { ++backup_calls; return allow_backup; }
  int backup_calls{};
  bool allow_backup{true};
  int login_calls{};
  bool allow_login{true};
  int configuration_calls{};
  bool allow_configuration{true};
  int close_calls{};
};

RecordingCloseSink g_close_sink;

[[nodiscard]] ComPtr<IDispatch> make_host(
    CardProductUiSessionController& controller) {
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              controller, &g_close_sink, &host)),
          "product host creation failed");
  return host;
}

[[nodiscard]] DISPID member_id(IDispatch& host, wchar_t* const name) {
  LPOLESTR mutable_name = name;
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &mutable_name, 1,
                                       LOCALE_INVARIANT, &id)),
          "product host member was not exposed");
  return id;
}

[[nodiscard]] DISPID member_id(IDispatch& host, const wchar_t* const name) {
  return member_id(host, const_cast<wchar_t*>(name));
}

[[nodiscard]] HRESULT invoke_login(IDispatch& host, VARIANTARG& password,
                                   UINT* const argument_error = nullptr) {
  DISPPARAMS parameters{&password, nullptr, 1, 0};
  return host.Invoke(member_id(host, L"submitLoginPasswordBytes"), IID_NULL,
                     LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                     nullptr, argument_error);
}

[[nodiscard]] HRESULT invoke_initialization(
    IDispatch& host, InitializationArguments& arguments,
    UINT* const argument_error = nullptr) {
  DISPPARAMS parameters = arguments.parameters();
  return host.Invoke(
      member_id(host, L"submitInitializationPasswordBytes"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr,
      argument_error);
}

[[nodiscard]] LONG invoke_state(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getSessionState"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "product state query failed");
  require(result.vt == VT_I4, "product state returned the wrong type");
  const LONG state = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return state;
}

[[nodiscard]] bool invoke_mounted(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"hasActiveSession"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "mounted query failed");
  require(result.vt == VT_BOOL, "mounted query returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] VARIANTARG number_argument(const std::uint32_t value) noexcept {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_UI4;
  argument.ulVal = value;
  return argument;
}

[[nodiscard]] HRESULT invoke_with_number(IDispatch& host,
                                         const wchar_t* const name,
                                         const std::uint32_t value,
                                         VARIANT* const result,
                                         UINT* const argument_error) {
  VARIANTARG argument = number_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr,
                     argument_error);
}

[[nodiscard]] VARIANTARG boolean_argument(const bool value) noexcept {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_BOOL;
  argument.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return argument;
}

[[nodiscard]] bool invoke_setting(IDispatch& host,
                                  const wchar_t* const name) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, name), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "setting query failed");
  require(result.vt == VT_BOOL, "a setting query returned the wrong type");
  const bool value = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] HRESULT invoke_set_setting(IDispatch& host,
                                         const wchar_t* const name,
                                         const bool value,
                                         UINT* const argument_error) {
  VARIANTARG argument = boolean_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  const HRESULT status =
      host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                  DISPATCH_METHOD, &parameters, nullptr, nullptr,
                  argument_error);
  static_cast<void>(::VariantClear(&argument));
  return status;
}

[[nodiscard]] bool invoke_remember(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getRememberContainerPaths"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "remember-paths query failed");
  require(result.vt == VT_BOOL,
          "remember-paths query returned the wrong type");
  const bool remember = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return remember;
}

[[nodiscard]] HRESULT invoke_set_remember(IDispatch& host, const bool value,
                                          UINT* const argument_error) {
  VARIANTARG argument = boolean_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  const HRESULT status =
      host.Invoke(member_id(host, L"setRememberContainerPaths"), IID_NULL,
                  LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                  nullptr, argument_error);
  static_cast<void>(::VariantClear(&argument));
  return status;
}

// A BSTR argument. The caller owns it and clears it with the rest of the list.
[[nodiscard]] VARIANTARG text_argument(const std::wstring& value) {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_BSTR;
  argument.bstrVal = ::SysAllocStringLen(value.c_str(),
                                         static_cast<UINT>(value.size()));
  require(argument.bstrVal != nullptr, "test BSTR allocation failed");
  return argument;
}

// `createContainerMib(name, directory, sizeMib)` with COM's reverse order.
[[nodiscard]] HRESULT invoke_create_container(
    IDispatch& host, const std::wstring& name, const std::wstring& directory,
    const std::uint32_t size_mib, VARIANT* const result,
    UINT* const argument_error, bool background = false) {
  std::array<VARIANTARG, 3> values{};
  values[2] = text_argument(name);
  values[1] = text_argument(directory);
  values[0] = number_argument(size_mib);
  DISPPARAMS parameters{values.data(), nullptr, 3, 0};
  const HRESULT status =
      host.Invoke(member_id(host, background ? L"beginCreateContainerMib" : L"createContainerMib"), IID_NULL,
                  LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, result,
                  nullptr, argument_error);
  for (VARIANTARG& value : values) {
    static_cast<void>(::VariantClear(&value));
  }
  return status;
}

[[nodiscard]] bool create_container_succeeds(IDispatch& host,
                                             const std::wstring& name,
                                             const std::wstring& directory,
                                             const std::uint32_t size_mib) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_create_container(host, name, directory, size_mib,
                                            &result, nullptr)),
          "container creation call failed");
  require(result.vt == VT_BOOL, "creation returned the wrong type");
  const bool created = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return created;
}

[[nodiscard]] ULONG invoke_container_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerCount"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container count query failed");
  require(result.vt == VT_UI4, "container count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] ULONG invoke_operation_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getOperationCount"),
                                IID_NULL,
                                LOCALE_INVARIANT,
                                DISPATCH_METHOD,
                                &parameters,
                                &result,
                                nullptr,
                                nullptr)),
          "operation count query failed");
  require(result.vt == VT_UI4, "operation count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] LONG invoke_operation_number(IDispatch& host,
                                           const wchar_t* const name,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "operation numeric fact query failed");
  require(result.vt == VT_I4, "operation numeric fact returned the wrong type");
  const LONG value = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] double invoke_operation_time(IDispatch& host,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(
              host, L"getOperationCompletedAtUnixMs", index, &result, nullptr)),
          "operation completion time query failed");
  require(result.vt == VT_R8,
          "operation completion time returned the wrong type");
  const double value = result.dblVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] ULONG invoke_container_number(IDispatch& host,
                                            const wchar_t* const name,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container fact query failed");
  require(result.vt == VT_UI4, "container fact returned the wrong type");
  const ULONG value = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_container_mounted(IDispatch& host,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, L"getContainerMounted", index,
                                       &result, nullptr)),
          "container mounted query failed");
  require(result.vt == VT_BOOL, "container mounted returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] bool invoke_container_bool(IDispatch& host,
                                         const wchar_t* const name,
                                         const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container flag query failed");
  require(result.vt == VT_BOOL, "container flag returned the wrong type");
  const bool value = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] std::wstring invoke_container_text(IDispatch& host,
                                                 const wchar_t* const name,
                                                 const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container text query failed");
  require(result.vt == VT_BSTR && result.bstrVal != nullptr,
          "container text returned the wrong type");
  const std::wstring value(result.bstrVal, ::SysStringLen(result.bstrVal));
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_selection(IDispatch& host,
                                    const wchar_t* const name,
                                    const std::uint32_t value) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, value, &result, nullptr)),
          "container selection call failed");
  require(result.vt == VT_BOOL, "container selection returned the wrong type");
  const bool selected = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return selected;
}

[[nodiscard]] bool invoke_mount_container(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"mountContainer"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "container adoption call failed");
  require(result.vt == VT_BOOL, "container adoption returned the wrong type");
  const bool adopted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return adopted;
}

void test_background_creation_reports_progress_and_excludes_concurrent_calls() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.pause_creation.store(true);
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, nullptr, &work)), "background host creation failed");
  const HRESULT begun = invoke_create_container(*host.Get(), L"background", fixture.directory.wide(),
      64, nullptr, nullptr, true);
  for (int i = 0; !fixture.volume.creation_entered.load() && i < 1000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  DISPPARAMS no_arguments{};
  VARIANT snapshot{};
  const HRESULT progress_result = host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr);
  const bool live = SUCCEEDED(progress_result) && snapshot.vt == VT_BSTR &&
      std::wstring_view(snapshot.bstrVal) == L"1,3,41,0,3";
  ::VariantClear(&snapshot);
  const HRESULT blocked = host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr);
  const HRESULT reported = page_event_tests::send(*host.Get());
  fixture.volume.pause_creation.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(SUCCEEDED(begun) && live && blocked == E_PENDING && !work.running.load(),
      "background operation did not report live progress or exclude concurrent access");
  require(fixture.volume.create_calls == 1, "background creation ran more than once");
  require(reported == S_OK, "background creation blocked page reporting");
  require(invoke_state(*host.Get()) == 3, "completed job did not release the session");
  fixture.authentication.clear_authentication();
  require(host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr) == E_ACCESSDENIED,
      "completed progress leaked after authentication ended");
}

void fill_container_b_arguments(std::array<VARIANTARG, 6>& values,
                                const std::wstring& name,
                                const std::wstring& directory,
                                std::span<const std::byte> second);

HRESULT begin_creation_for_test(IDispatch& host, Fixture& fixture,
                                const std::wstring& name, const bool hidden) {
  if (!hidden) return invoke_create_container(host, name, fixture.directory.wide(),
      64, nullptr, nullptr, true);
  const auto password = bytes("second-password-value");
  std::array<VARIANTARG, 6> values{};
  fill_container_b_arguments(values, name, fixture.directory.wide(), password);
  DISPPARAMS parameters{values.data(), nullptr, 6, 0};
  const HRESULT result = host.Invoke(member_id(host, L"beginCreateContainerBMib"),
      IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr, nullptr);
  for (std::size_t i = 0; i < 2; ++i) {
    require_array_wiped(values[i], password.size(),
        "a cancellable hidden creation retained its marshalled password");
  }
  for (auto& value : values) ::VariantClear(&value);
  return result;
}

// `expected_code` is the value the page must read back, when the caller cares
// which one it is. Zero leaves it to the rule for the expected status.
void check_background_creation_outcome(const std::exception_ptr& failure,
                                       const bool request_cancel,
                                       const std::wstring_view expected_status,
                                       const bool skip_checkpoint = false,
                                       const bool hidden = false,
                                       const unsigned int expected_code = 0) {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.pause_creation.store(true);
  fixture.volume.creation_failure = failure;
  fixture.volume.skip_cancel_checkpoint = skip_checkpoint;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, nullptr, &work)),
      "cancellable host creation failed");
  const HRESULT begun = begin_creation_for_test(*host.Get(), fixture, L"called-off", hidden);
  for (int i = 0; !fixture.volume.creation_entered.load() && i < 1000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  DISPPARAMS no_arguments{};
  VARIANT ignored{};
  // Calling a creation off is one of the two things it leaves reachable; the
  // busy guard would answer E_PENDING to anything else.
  const HRESULT asked = request_cancel ? host->Invoke(
      member_id(*host.Get(), L"cancelContainerCreation"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &ignored, nullptr,
      nullptr) : S_OK;
  const bool accepted = !request_cancel ||
      (ignored.vt == VT_BOOL && ignored.boolVal == VARIANT_TRUE);
  ::VariantClear(&ignored);
  fixture.volume.pause_creation.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  VARIANT snapshot{};
  const HRESULT progress_result = host->Invoke(
      member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr,
      nullptr);
  const bool expected_outcome = SUCCEEDED(progress_result) &&
      snapshot.vt == VT_BSTR &&
      std::wstring_view(snapshot.bstrVal).starts_with(expected_status);
  ::VariantClear(&snapshot);
  require(SUCCEEDED(begun) && SUCCEEDED(asked) &&
          accepted && !work.cancellation_requested() &&
          expected_outcome && !work.running.load(),
          "background creation misclassified cancellation or cleanup failure");
  require(fixture.volume.containers.empty(), "failed or cancelled creation was adopted");
  VARIANT failure_code{};
  require(SUCCEEDED(host->Invoke(member_id(*host.Get(), L"getCreationError"), IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &failure_code, nullptr, nullptr)) &&
      failure_code.vt == VT_UI4 && failure_code.ulVal != 0, "creation failure code missing");
  if (expected_status == L"4,") require(failure_code.ulVal == 16, "cancellation code lost");
  if (expected_status == L"5,") require(failure_code.ulVal == 15 || failure_code.ulVal == static_cast<unsigned int>(E_FAIL), "cleanup code lost");
  if (expected_code)
    require(failure_code.ulVal == expected_code,
            "the page would read a creation failure code that is not the one");
  else if (expected_status == L"3,")
    require(failure_code.ulVal == static_cast<unsigned int>(E_FAIL),
            "a failure outside the creation vocabulary was reported as one");
  ::VariantClear(&failure_code);

  // Reuse the same host, name and authenticated session. No stale cancellation
  // or terminal status may prevent the next operation from completing.
  fixture.volume.creation_failure = {};
  fixture.volume.creation_entered.store(false);
  const HRESULT retried = begin_creation_for_test(*host.Get(), fixture, L"called-off", hidden);
  for (int i = 0; work.running.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const HRESULT retry_progress = host->Invoke(
      member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr);
  const bool retry_succeeded = SUCCEEDED(retry_progress) && snapshot.vt == VT_BSTR &&
      std::wstring_view(snapshot.bstrVal).starts_with(L"2,");
  ::VariantClear(&snapshot);
  require(SUCCEEDED(retried) && !work.running.load() && retry_succeeded &&
      fixture.volume.containers.size() == 1, "a previous cancellation poisoned the next run");
}

void test_a_running_creation_can_be_called_off() {
  using usb_vault::volume_creation::CreationError;
  using usb_vault::volume_creation::make_error_code;
  for (const bool hidden : {false, true}) {
    check_background_creation_outcome({}, true, L"4,", false, hidden);
    // Cancellation arrived after the session's last query but before its
    // publication feedback. The atomic gate must still stop publication.
    check_background_creation_outcome({}, true, L"4,", true, hidden);
    const auto cancelled = std::make_exception_ptr(
        std::system_error(make_error_code(CreationError::operation_cancelled)));
    const auto cleanup_failed = std::make_exception_ptr(
        std::system_error(make_error_code(CreationError::cleanup_failed)));
    const auto combined = std::make_exception_ptr(
        usb_vault::application::ContainerCreationCleanupError(
            cancelled, std::make_exception_ptr(std::runtime_error("cleanup failed"))));
    for (const bool requested : {false, true}) {
      check_background_creation_outcome(cleanup_failed, requested, L"5,", false, hidden);
      check_background_creation_outcome(combined, requested, L"5,", false, hidden);
      check_background_creation_outcome(std::make_exception_ptr(
          std::runtime_error("unrelated failure")), requested, L"3,", false, hidden);
      // Same numeric value in another category must not count as cancellation,
      // and must not be handed to the page as that creation error either.
      check_background_creation_outcome(std::make_exception_ptr(
          std::system_error(std::error_code(
              static_cast<int>(CreationError::operation_cancelled),
              std::generic_category()))), requested, L"3,", false, hidden);
      // A name already taken arrives as errc::file_exists, whose value is the
      // one io_error uses. It has an exact counterpart, so it must arrive as
      // that one rather than as a read-write failure.
      check_background_creation_outcome(std::make_exception_ptr(
          std::system_error(std::make_error_code(std::errc::file_exists))),
          requested, L"3,", false, hidden,
          static_cast<unsigned int>(CreationError::target_exists));
    }
    check_background_creation_outcome(cancelled, false, L"4,", false, hidden);
  }
}

void test_creation_finalization_rejects_late_cancellation() {
  for (const bool hidden : {false, true}) {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.pause_finalization.store(true);
    usb_vault::ui::CreationWorkState work;
    ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(
        fixture.controller, nullptr, &host, nullptr, &work)), "host creation failed");
    const HRESULT begun = begin_creation_for_test(*host.Get(), fixture, L"finalizing", hidden);
    for (int i = 0; !fixture.volume.finalization_entered.load() && i < 1000; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DISPPARAMS args{};
    VARIANT answer{};
    const HRESULT asked = host->Invoke(member_id(*host.Get(), L"cancelContainerCreation"),
        IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &args, &answer, nullptr, nullptr);
    const bool rejected = SUCCEEDED(asked) && answer.vt == VT_BOOL &&
        answer.boolVal == VARIANT_FALSE;
    ::VariantClear(&answer);
    // Window closure uses the same gate, even while its question was open.
    const bool window_rejected = !work.can_cancel() && !work.request_cancel();
    fixture.volume.pause_finalization.store(false);
    for (int i = 0; work.running.load() && i < 2000; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const HRESULT progress = host->Invoke(member_id(*host.Get(), L"getCreationProgress"),
        IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &args, &answer, nullptr, nullptr);
    const bool succeeded = SUCCEEDED(progress) && answer.vt == VT_BSTR &&
        std::wstring_view(answer.bstrVal).starts_with(L"2,");
    ::VariantClear(&answer);
    require(SUCCEEDED(begun) && fixture.volume.finalization_entered.load() &&
        rejected && window_rejected && succeeded && !work.running.load() &&
        !work.request_cancel(), "late cancellation was accepted or changed the outcome");
  }
}

void test_operation_logging_setting_is_authenticated_and_boolean() {
  {
    Fixture preference_fixture;
    auto preference_host = make_host(preference_fixture.controller);
    require(FAILED(invoke_set_setting(*preference_host.Get(), L"setForceUnmountEnabled", true, nullptr)), "unauthenticated force preference accepted");
    require(FAILED(invoke_set_setting(*preference_host.Get(), L"setAutoMountEnabled", true, nullptr)), "unauthenticated auto preference accepted");
    preference_fixture.authentication.establish(root_secret());
    require(!invoke_setting(*preference_host.Get(), L"getAutoMountEnabled"), "auto mount default changed");
    require(SUCCEEDED(invoke_set_setting(*preference_host.Get(), L"setAutoMountEnabled", true, nullptr)) &&
            invoke_setting(*preference_host.Get(), L"getAutoMountEnabled") && preference_fixture.volume.batch_calls.empty(), "auto preference started work or was not saved");
    require(invoke_with_number(*preference_host.Get(), L"setAutoMountEnabled", 1, nullptr, nullptr) == DISP_E_TYPEMISMATCH,
            "non-boolean auto preference accepted");
    require(SUCCEEDED(invoke_set_setting(*preference_host.Get(), L"setForceUnmountEnabled", true, nullptr)) &&
            preference_fixture.volume.force_preference, "force preference was not saved");
    require(invoke_with_number(*preference_host.Get(), L"setForceUnmountEnabled", 1, nullptr, nullptr) == DISP_E_TYPEMISMATCH,
            "non-boolean force preference admitted");
  }
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(FAILED(invoke_set_setting(*host.Get(), L"setOperationLoggingEnabled",
      false, nullptr)), "unauthenticated logging setting accepted");
  fixture.authentication.establish(root_secret());
  require(invoke_setting(*host.Get(), L"getOperationLoggingEnabled"),
      "logging default was not projected");
  require(SUCCEEDED(invoke_set_setting(*host.Get(), L"setOperationLoggingEnabled",
      false, nullptr)) && !fixture.volume.logging_enabled,
      "logging preference did not reach the session");
  require(invoke_with_number(*host.Get(), L"setOperationLoggingEnabled", 1,
      nullptr, nullptr) == DISP_E_TYPEMISMATCH,
      "non-boolean logging setting accepted");
}

void test_exit_hotkey_settings_bridge() {
  class Sink final : public usb_vault::ui::IUiExitHotkeySink {
   public:
    usb_vault::application::ExitHotkeySettings exit_hotkey() const noexcept override { return value; }
    bool set_exit_hotkey(usb_vault::application::ExitHotkeySettings next) noexcept override {
      ++calls; if (!succeeds) return false; value = next; return true;
    }
    usb_vault::application::ExitHotkeySettings value;
    void set_capture(bool active) noexcept override { capturing = active; }
    bool capturing{};
    unsigned int calls{};
    bool succeeds{true};
  } sink;
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host,
      nullptr, &work, nullptr, nullptr, &sink)), "shortcut host failed");
  VARIANTARG capture_arg{}; capture_arg.vt = VT_BOOL; capture_arg.boolVal = VARIANT_TRUE;
  DISPPARAMS capture_params{&capture_arg, nullptr, 1, 0};
  auto capture = [&] { return host->Invoke(member_id(*host.Get(), L"setExitHotkeyCapture"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &capture_params, nullptr, nullptr, nullptr); };
  require(capture() == S_OK && sink.capturing, "capture not enabled");
  work.running.store(true); capture_arg.boolVal = VARIANT_FALSE;
  require(capture() == S_OK && !sink.capturing, "busy work prevented capture release");
  work.running.store(false); capture_arg.vt = VT_I4;
  require(capture() == DISP_E_TYPEMISMATCH, "invalid capture type accepted");
  VARIANTARG args[3]{};
  args[0].vt = VT_UI4; args[0].ulVal = 3;
  args[1].vt = VT_UI4; args[1].ulVal = 123;
  args[2].vt = VT_BOOL; args[2].boolVal = VARIANT_TRUE;
  DISPPARAMS params{args, nullptr, 3, 0};
  auto set = [&] { return host->Invoke(member_id(*host.Get(), L"setExitHotkey"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &params, nullptr, nullptr, nullptr); };
  require(set() == E_ACCESSDENIED && sink.calls == 0, "unauthenticated shortcut change accepted");
  fixture.authentication.establish(root_secret());
  require(set() == S_OK && sink.value.enabled, "valid shortcut rejected");
  args[0].ulVal = 0; require(set() == E_INVALIDARG, "zero count accepted");
  args[0].ulVal = 11; require(set() == E_INVALIDARG, "excess count accepted");
  args[0].ulVal = 3; args[1].ulVal = 17; require(set() == E_INVALIDARG, "modifier accepted");
  args[1].ulVal = 123; args[2].vt = VT_I4; require(set() == DISP_E_TYPEMISMATCH, "nonboolean enable accepted");
  args[2].vt = VT_BOOL;
  work.running.store(true); require(set() == E_PENDING, "busy shortcut change accepted");
  work.running.store(false); work.hotkey_exit_pending.store(true);
  DISPPARAMS query{};
  require(set() == E_PENDING && host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &query, nullptr, nullptr, nullptr) == E_PENDING,
      "exit did not exclude session access");
  work.hotkey_exit_pending.store(false);
  sink.succeeds = false; require(set() == E_FAIL, "runtime shortcut failure hidden");
  DISPPARAMS none{}; VARIANT result{};
  require(SUCCEEDED(host->Invoke(member_id(*host.Get(), L"getExitHotkey"), IID_NULL, LOCALE_INVARIANT,
      DISPATCH_METHOD, &none, &result, nullptr, nullptr)) && result.vt == VT_UI4 && result.ulVal == 66427,
      "shortcut readback incorrect");
  ::VariantClear(&result);
}

void test_file_visibility_bridge_guards() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work)), "visibility host failed");
  VARIANTARG args[2]{};
  args[0].vt = VT_BOOL; args[0].boolVal = VARIANT_TRUE;
  args[1].vt = VT_UI4; args[1].ulVal = 7;
  const auto invoke = [&](const wchar_t* name, UINT count) {
    DISPPARAMS params{args, nullptr, count, 0};
    return host->Invoke(member_id(*host.Get(), name), IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &params, nullptr, nullptr, nullptr);
  };
  require(invoke(L"setHideFileContainers", 1) == E_ACCESSDENIED && invoke(L"setContainerFileHidden", 2) == E_ACCESSDENIED, "unauthenticated visibility accepted");
  VARIANT result{};
  require(invoke_with_number(*host.Get(), L"getContainerFileHidden", 7, &result, nullptr) == E_ACCESSDENIED, "unauthenticated file state read accepted");
  fixture.authentication.establish(root_secret());
  require(invoke_with_number(*host.Get(), L"getContainerFileHidden", 7, &result, nullptr) == S_OK && result.vt == VT_BOOL && result.boolVal == VARIANT_FALSE, "file state readback failed");
  ::VariantClear(&result);
  require(invoke(L"setHideFileContainers", 1) == S_OK && fixture.volume.file_hiding, "visibility preference failed");
  require(invoke(L"setContainerFileHidden", 2) == S_OK && fixture.volume.visibility_id == 7, "visibility id/boolean reversed");
  args[0].vt = VT_I4;
  require(invoke(L"setHideFileContainers", 1) == DISP_E_TYPEMISMATCH, "numeric boolean admitted");
  args[0].vt = VT_BOOL;
  args[1].ulVal = 0;
  require(invoke(L"setContainerFileHidden", 2) == DISP_E_TYPEMISMATCH, "zero visibility id admitted");
  fixture.volume.extension_save_fails = true;
  require(invoke(L"setHideFileContainers", 1) == E_FAIL, "failed setting write fabricated success");
  work.running.store(true);
  args[1].ulVal = 8;
  require(invoke(L"setContainerFileHidden", 2) == E_PENDING && fixture.volume.visibility_id == 7, "worker visibility mutation admitted");
}

void test_container_extension_preference_is_authenticated_and_bounded() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, nullptr, &work)), "extension host creation failed");
  DISPPARAMS none{};
  const auto read = [&](VARIANT& result) {
    return host->Invoke(member_id(*host.Get(), L"getContainerExtension"), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &none, &result, nullptr, nullptr);
  };
  VARIANT result{};
  require(read(result) == E_ACCESSDENIED &&
          invoke_with_number(*host.Get(), L"setContainerExtension", 0, nullptr, nullptr) == E_ACCESSDENIED,
          "unauthenticated extension preference admitted");
  fixture.authentication.establish(root_secret());
  for (int value = 0; value < 4; ++value) {
    require(invoke_with_number(*host.Get(), L"setContainerExtension", value, nullptr, nullptr) == S_OK &&
            read(result) == S_OK && result.vt == VT_UI4 && result.ulVal == static_cast<unsigned int>(value),
            "extension preference did not round trip");
    ::VariantClear(&result);
  }
  for (const double invalid : {-1.0, 4.0, 0.5, 4294967296.0}) {
    VARIANTARG arg{}; arg.vt = VT_R8; arg.dblVal = invalid;
    DISPPARAMS parameters{&arg, nullptr, 1, 0};
    require(host->Invoke(member_id(*host.Get(), L"setContainerExtension"), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr, nullptr) == DISP_E_TYPEMISMATCH,
        "unsupported extension selection admitted");
  }
  fixture.volume.extension_save_fails = true;
  require(invoke_with_number(*host.Get(), L"setContainerExtension", 0, nullptr, nullptr) == E_FAIL &&
          fixture.volume.extension_preference == 3, "failed extension write fabricated success");
  fixture.volume.extension_save_fails = false;
  work.running.store(true);
  require(invoke_with_number(*host.Get(), L"setContainerExtension", 0, nullptr, nullptr) == E_PENDING &&
          fixture.volume.extension_preference == 3, "busy worker allowed extension mutation");
  work.running.store(false);
  require(invoke_with_number(*host.Get(), L"setContainerExtension", 0, nullptr, nullptr) == S_OK,
          "fixed sys preference rejected");
  // The retired preference no longer names files: creation is GUID-named.
  require(create_container_succeeds(*host.Get(), L"cache.hc", fixture.directory.wide(), 64) &&
          fixture.directory.holds_protected(fixture.volume.created_path) &&
          fixture.volume.created_display_name == L"cache",
          "stored extension leaked into protected creation");
}

void test_window_size_presets_are_bounded() {
  class SizeSink final : public usb_vault::ui::IUiWindowSizeSink {
   public:
    unsigned int window_size() const noexcept override { return selected; }
    bool set_window_size(unsigned int preset) noexcept override {
      selected = preset;
      ++calls;
      return succeeds;
    }
    unsigned int selected{};
    int calls{};
    bool succeeds{true};
  } sink;
  Fixture fixture;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, &sink)), "size host creation failed");
  for (int preset = 0; preset < 4; ++preset) {
    require(SUCCEEDED(invoke_with_number(*host.Get(), L"setWindowSize",
        preset, nullptr, nullptr)) && sink.selected == static_cast<unsigned int>(preset),
        "valid size preset was not applied");
  }
  for (const double invalid : {-1.0, 4.0, 0.5, 4294967296.0}) {
    VARIANTARG argument{};
    argument.vt = VT_R8;
    argument.dblVal = invalid;
    DISPPARAMS parameters{&argument, nullptr, 1, 0};
    require(host->Invoke(member_id(*host.Get(), L"setWindowSize"), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr,
        nullptr) == DISP_E_TYPEMISMATCH, "invalid size accepted");
  }
  require(sink.calls == 4, "invalid size reached window sink");
  DISPPARAMS no_size_arguments{};
  VARIANT size_result{};
  require(host->Invoke(member_id(*host.Get(), L"getWindowSize"), IID_NULL, LOCALE_INVARIANT,
          DISPATCH_METHOD, &no_size_arguments, &size_result, nullptr, nullptr) == S_OK &&
          size_result.vt == VT_UI4 && size_result.ulVal == 3, "window preset was not restored to the page");
  sink.succeeds = false;
  require(FAILED(invoke_with_number(*host.Get(), L"setWindowSize", 0,
      nullptr, nullptr)), "window failure was hidden");
  host = make_host(fixture.controller);
  require(FAILED(invoke_with_number(*host.Get(), L"setWindowSize", 0,
      nullptr, nullptr)), "missing size sink claimed success");
}

void test_surface_and_state_projection_are_fixed() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  for (const auto [route, expected] : {
           std::pair{std::optional<CardEntryRoute>{}, 0L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::initialization_required},
                     1L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::login_required},
                     2L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::unavailable},
                     4L}}) {
    fixture.entry.route = route;
    require(page_event_tests::send(*host.Get()) == S_OK, "unauthenticated state blocked page reporting");
    require(invoke_state(*host.Get()) == expected,
            "product route mapped to the wrong wire state");
  }
  fixture.authentication.establish(root_secret());
  require(invoke_state(*host.Get()) == 3L,
          "authenticated state mapped to the wrong wire value");
  page_event_tests::verify_contract(*host.Get());

  for (const wchar_t* const forbidden : {
           L"submitPasswordBytes", L"hardwareId", L"credentialRole",
           L"mode", L"tier", L"containerShape", L"volumeType",
           L"rootSecret", L"beginMountAll"}) {
    LPOLESTR name = const_cast<LPOLESTR>(forbidden);
    DISPID id = 0;
    require(host->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT, &id) ==
                    DISP_E_UNKNOWNNAME &&
                id == DISPID_UNKNOWN,
            "product host exposed a forbidden member");
  }
}

// Page calls reach the runtime log with their name, arguments and result. A
// password array is recorded by size alone, and a successful poll is not
// recorded at all.
void test_page_reports_share_quota() {
  class ConfigurationHost final : public usb_vault::ui::IConfigurationHost {
   public:
    usb_vault::ui::ConfigurationCredentialStatus credential_status() noexcept override {
      throw_if_called = true;
      return usb_vault::ui::ConfigurationCredentialStatus::missing;
    }
    usb_vault::ui::ConfigurationOutcome save_credentials(std::span<const std::byte>,
        std::span<const std::byte>, std::span<const std::byte>) noexcept override {
      throw_if_called = true;
      return usb_vault::ui::ConfigurationOutcome::storage_failure;
    }
    bool continue_to_session() noexcept override { throw_if_called = true; return false; }
    bool throw_if_called = false;
  } configuration;
  Fixture fixture;
  auto product = make_host(fixture.controller);
  ComPtr<IDispatch> setup;
  require(SUCCEEDED(usb_vault::ui::create_card_configuration_webview_host_object(configuration, &setup)),
          "shared quota configuration host failed");
  page_event_tests::Log log;
  const auto initial = log.read();
  for (int i = 0; i < 204; ++i)
    require(page_event_tests::send(i % 2 ? *product.Get() : *setup.Get()) == S_OK,
            "shared page quota changed reporting result");
  log.file.close();
#if USB_VAULT_DEBUG_LOG
  const auto text = log.read();
  require(std::count(text.begin(), text.end(), '\n') == 202 &&
          text.ends_with("[页面] 限流；已丢弃 4 条\r\n"), "bridges did not share their page quota");
#else
  require(log.read() == initial, "disabled bridges wrote page reports");
#endif
  require(!configuration.throw_if_called, "page report touched configuration state");
}

void test_runtime_log_records_page_calls() {
#if USB_VAULT_DEBUG_LOG
  namespace diagnostics = usb_vault::diagnostics;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      (L"usb-vault-bridge-log-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
       std::to_wstring(::GetTickCount64()));
  require(std::filesystem::create_directory(root), "the bridge log fixture already exists");
  const SYSTEMTIME time{2026, 9, 0, 26, 12, 0, 0, 0};
  const std::filesystem::path path = root / L"logs" / L"runtime-log-20260926-120000.txt";
  {
    diagnostics::DebugLogFile file;
    require(file.open(root, time), "cannot open the bridge log fixture");
    diagnostics::set_runtime_log(&file);
    struct Withdraw final {
      ~Withdraw() { usb_vault::diagnostics::set_runtime_log(nullptr); }
    } withdraw;
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    std::vector<std::byte> password{std::byte{'s'}, std::byte{'e'}, std::byte{'k'},
                                    std::byte{'r'}, std::byte{'i'}, std::byte{'t'}};
    ByteArray argument(password);
    require(invoke_login(*host.Get(), argument.value()) == S_OK, "the logged login failed");
    static_cast<void>(invoke_state(*host.Get()));
    // Already authenticated: the controller refuses and the bridge swallows
    // the exception into a failed HRESULT. The log keeps the exception.
    ByteArray again(password);
    require(FAILED(invoke_login(*host.Get(), again.value())), "a second login was accepted");
  }
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "the bridge log was not written");
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  require(text.find("[页面桥] 页面调用 submitLoginPasswordBytes；参数 字节数组 6 项；HRESULT 0x00000000") !=
              std::string::npos,
          "the login call was not recorded with its argument size and result");
  require(text.find("[登录] 开始登录；输入长度 6 字节") != std::string::npos &&
              text.find("[登录] 登录成功；状态 ") != std::string::npos,
          "the controller did not record the login");
  require(text.find("页面调用 getSessionState") == std::string::npos,
          "a successful poll was recorded");
  const auto refused = text.find("[登录] 当前状态不能登录；状态 ");
  const auto handled = text.find("[ui/card_product_webview_host_object] 异常已处理；行 ", refused);
  const auto failed = text.find("页面调用 submitLoginPasswordBytes；参数 字节数组 6 项；HRESULT 0x8", handled);
  require(refused != std::string::npos && handled != std::string::npos && failed != std::string::npos &&
              text.find("异常类型：", handled) < failed,
          "a swallowed bridge exception was not recorded before its failed HRESULT");
  require(text.find("sekrit") == std::string::npos, "a password reached the bridge log");
  std::filesystem::remove(path);
  std::filesystem::remove(root / L"logs");
  std::filesystem::remove(root);
#endif
}

void test_login_consumes_and_wipes_one_array() {
  for (const bool reject : {false, true}) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    fixture.entry.reject_credential = reject;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    std::vector<std::byte> password{
        std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}};
    ByteArray argument(password);

    const HRESULT result = invoke_login(*host.Get(), argument.value());
    argument.require_wiped("login SAFEARRAY survived host invocation");
    require(fixture.entry.login_calls == 1,
            "login host did not forward exactly once");
    if (reject) {
      require(result == E_FAIL && invoke_state(*host.Get()) == 2L,
              "credential rejection lost the retryable login state");
    } else {
      const std::array expected{std::byte{0xC3}, std::byte{0xA9}};
      require(result == S_OK &&
                  equals(fixture.volume.login_bytes, expected) &&
                  invoke_state(*host.Get()) == 3L &&
                  !invoke_mounted(*host.Get()),
              "login host changed NFC or state, or mounted at login");
    }
  }
}

void test_initialization_preserves_com_order_and_wipes_both_arrays() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  std::vector<std::byte> password = bytes("aaaaaaaaaaae");
  password.push_back(std::byte{0xCC});
  password.push_back(std::byte{0x81});
  std::vector<std::byte> confirmation = bytes("aaaaaaaaaaa");
  confirmation.push_back(std::byte{0xC3});
  confirmation.push_back(std::byte{0xA9});
  InitializationArguments arguments(password, confirmation);

  require(invoke_initialization(*host.Get(), arguments) == S_OK,
          "initialization host rejected equivalent NFC inputs");
  arguments.require_password_wiped();
  arguments.require_confirmation_wiped();
  std::vector<std::byte> expected = bytes("aaaaaaaaaaa");
  expected.push_back(std::byte{0xC3});
  expected.push_back(std::byte{0xA9});
  require(fixture.entry.initialization_calls == 1 &&
              equals(fixture.entry.initialization_bytes, expected) &&
              invoke_state(*host.Get()) == 3L,
          "initialization host reversed, changed, or failed inputs");
}

void test_initialization_failure_wipes_every_supported_argument() {
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("bbbbbbbbbbbb");
    InitializationArguments arguments(password, confirmation);

    require(invoke_initialization(*host.Get(), arguments) == E_FAIL,
            "mismatched initialization inputs were accepted");
    arguments.require_password_wiped();
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "mismatched inputs reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_password_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 1,
            "invalid password argument returned the wrong COM result");
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid password argument reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_confirmation_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 0,
            "invalid confirmation returned the wrong COM result");
    arguments.require_password_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid confirmation reached the card action");
  }
}

void test_bad_arity_still_wipes_supplied_password_array() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto password = bytes("aaaaaaaaaaaa");
  ByteArray argument(password);
  DISPPARAMS parameters{&argument.value(), nullptr, 1, 0};

  require(host->Invoke(member_id(*host.Get(),
                                 L"submitInitializationPasswordBytes"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "initialization host accepted the wrong argument count");
  argument.require_wiped("bad-arity password SAFEARRAY survived");
}

void test_unmount_and_property_rejection() {
  Fixture fixture;
  fixture.volume.mounted_state = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};

  require(invoke_mounted(*host.Get()), "host hid active volume state");
  require(host->Invoke(member_id(*host.Get(), L"unmount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                       nullptr, nullptr) == S_OK &&
              fixture.volume.unmount_calls == 1 &&
              !invoke_mounted(*host.Get()),
          "host did not forward unified unmount");
  require(host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_PROPERTYGET, &parameters,
                       nullptr, nullptr, nullptr) == DISP_E_MEMBERNOTFOUND,
          "product state was exposed as a property");
}

void test_container_collection_is_exposed_by_position_and_route_gated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_container_count(*host.Get()) == 0UL,
          "unauthenticated host exposed adopted containers");

  fixture.authentication.establish(root_secret());
  require(create_container_succeeds(*host.Get(), L"work",
                                    fixture.directory.wide(), 64) &&
              fixture.chooser.directory_calls == 0 &&
              fixture.volume.create_calls == 1 &&
              fixture.directory.holds_protected(fixture.volume.created_path) &&
              fixture.volume.created_display_name == L"work" &&
              fixture.volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL,
          "creation did not carry the named location to the session once");

  require(invoke_container_count(*host.Get()) == 1UL &&
              invoke_container_number(*host.Get(), L"getContainerId", 0) ==
                  1UL &&
              invoke_container_text(*host.Get(), L"getContainerName", 0) ==
                  L"work" &&
              invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_number(*host.Get(), L"getContainerSizeMib",
                                      0) == 64UL &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0) == L"D:" &&
              !invoke_container_bool(*host.Get(), L"getContainerVolumeUnavailable", 0),
          "host lost an adopted container fact");

  // A volume the session could not read is still one row with its letter.
  fixture.volume.containers[0].unavailable = true;
  require(invoke_container_count(*host.Get()) == 1UL &&
              invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_bool(*host.Get(), L"getContainerVolumeUnavailable", 0) &&
              invoke_container_number(*host.Get(), L"getContainerSizeMib", 0) == 0UL &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter", 0) == L"D:",
          "host did not report an unreadable volume as its own row");
  fixture.volume.containers[0].unavailable = false;

  VARIANT result{};
  ::VariantInit(&result);
  require(invoke_with_number(*host.Get(), L"getContainerName", 1, &result,
                             nullptr) == E_INVALIDARG,
          "host answered a position no container occupies");
  static_cast<void>(::VariantClear(&result));
}

void test_container_calls_reject_wrong_shapes_and_report_cancellation() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::wstring directory = fixture.directory.wide();
  ComPtr<IDispatch> host = make_host(fixture.controller);

  UINT argument_error = 99;
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "host accepted a creation call without any arguments");
  require(invoke_with_number(*host.Get(), L"createContainerMib", 64, nullptr,
                             &argument_error) == DISP_E_BADPARAMCOUNT,
          "host accepted a creation call carrying only a capacity");

  // The two new arguments are strings and nothing else. A number where a name
  // belongs is not coerced into one.
  std::array<VARIANTARG, 3> numeric{};
  numeric[2] = number_argument(7);
  numeric[1] = number_argument(8);
  numeric[0] = number_argument(64);
  DISPPARAMS numeric_parameters{numeric.data(), nullptr, 3, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &numeric_parameters, nullptr, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host accepted a number where only a name is allowed");
  for (VARIANTARG& value : numeric) {
    static_cast<void>(::VariantClear(&value));
  }

  require(invoke_create_container(*host.Get(), L"work", directory, 19, nullptr,
                                  &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity below the shared minimum");

  // The page owns the location now, so the shapes it must not be able to
  // express are refused here rather than reaching the session.
  for (const std::pair<std::wstring, std::wstring> rejected : {
           std::pair<std::wstring, std::wstring>{L"", directory},
           std::pair<std::wstring, std::wstring>{L"..\\escape", directory},
           std::pair<std::wstring, std::wstring>{L"work", L"Vault"},
           std::pair<std::wstring, std::wstring>{L"work",
                                                 L"\\\\server\\share"},
           std::pair<std::wstring, std::wstring>{L"work",
                                                 directory + L"\\missing"}}) {
    require(invoke_create_container(*host.Get(), rejected.first,
                                    rejected.second, 64, nullptr,
                                    &argument_error) == DISP_E_TYPEMISMATCH,
            "host accepted a location the page must not be able to name");
  }

  std::array<VARIANTARG, 3> fractional{};
  fractional[2] = text_argument(L"work");
  fractional[1] = text_argument(directory);
  ::VariantInit(&fractional[0]);
  fractional[0].vt = VT_R8;
  fractional[0].dblVal = 64.5;
  DISPPARAMS fractional_parameters{fractional.data(), nullptr, 3, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &fractional_parameters, nullptr, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity that is not a whole number");
  for (VARIANTARG& value : fractional) {
    static_cast<void>(::VariantClear(&value));
  }

  require(fixture.chooser.directory_calls == 0 &&
              fixture.volume.create_calls == 0,
          "a rejected creation still opened a dialog or the session");

  require(!invoke_mount_container(*host.Get()) &&
              fixture.chooser.existing_calls == 1,
          "a cancelled adoption was not reported as a cancellation");
}

// The browse button. It reports the choice as a string and creates nothing.
void test_directory_browse_reports_the_choice_as_a_string() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  fixture.chooser.directory_path = L"D:\\Vault";
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, &result, nullptr, nullptr) == S_OK &&
              result.vt == VT_BSTR && result.bstrVal != nullptr &&
              std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal)) ==
                  L"D:\\Vault",
          "the chosen directory did not reach the page");
  static_cast<void>(::VariantClear(&result));

  fixture.chooser.directory_path.clear();
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, &result, nullptr, nullptr) == S_OK &&
              result.vt == VT_BSTR &&
              ::SysStringLen(result.bstrVal) == 0,
          "a cancelled picker was not reported as an empty string");
  static_cast<void>(::VariantClear(&result));

  VARIANTARG stray = number_argument(1);
  DISPPARAMS stray_parameters{&stray, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &stray_parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "the picker accepted an argument it does not take");
  static_cast<void>(::VariantClear(&stray));
  require(fixture.chooser.directory_calls == 2 &&
              fixture.volume.create_calls == 0,
          "browsing created something or ran the wrong number of times");
}

void test_the_remember_switch_crosses_the_bridge_as_a_boolean() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  // Reading needs no session: the settings page shows the switch in its real
  // position before anyone logs in.
  require(invoke_remember(*host.Get()), "the switch did not read its default");
  require(invoke_set_remember(*host.Get(), false, nullptr) == E_FAIL &&
              fixture.volume.remember_paths_writes == 0,
          "the switch was changed off the authenticated route");

  fixture.authentication.establish(root_secret());
  require(invoke_set_remember(*host.Get(), false, nullptr) == S_OK &&
              fixture.volume.remember_paths_writes == 1 &&
              !invoke_remember(*host.Get()),
          "turning the switch off did not reach the session");
  require(invoke_set_remember(*host.Get(), true, nullptr) == S_OK &&
              invoke_remember(*host.Get()),
          "the switch could not be turned back on");

  // A setting is a boolean and nothing else. A number that C++ would happily
  // read as true must not cross as one.
  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"setRememberContainerPaths", 1,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "the switch accepted a number");
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"setRememberContainerPaths"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "the switch accepted a call with nothing to set");
  VARIANTARG stray = boolean_argument(true);
  DISPPARAMS stray_parameters{&stray, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"getRememberContainerPaths"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &stray_parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "reading the switch accepted an argument it does not take");
  static_cast<void>(::VariantClear(&stray));
  require(fixture.volume.remember_paths_writes == 2,
          "a refused call still reached the session");
}

void test_the_import_switch_crosses_the_bridge_as_a_boolean() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  require(!invoke_setting(*host.Get(), L"getMountImportedContainers"),
          "the import switch did not read its default");
  require(invoke_set_setting(*host.Get(), L"setMountImportedContainers", true,
                             nullptr) == E_FAIL &&
              fixture.volume.mount_imported_writes == 0,
          "the switch was changed off the authenticated route");

  fixture.authentication.establish(root_secret());
  require(invoke_set_setting(*host.Get(), L"setMountImportedContainers", true,
                             nullptr) == S_OK &&
              fixture.volume.mount_imported_writes == 1 &&
              invoke_setting(*host.Get(), L"getMountImportedContainers"),
          "turning the switch on did not reach the session");

  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"setMountImportedContainers", 1,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "the switch accepted a number");
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"setMountImportedContainers"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "the switch accepted a call with nothing to set");
  require(fixture.volume.mount_imported_writes == 1,
          "a refused call still reached the session");
}

void test_background_volume_operations() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, &g_close_sink, &host, nullptr, &work)), "host creation failed");
  require(create_container_succeeds(*host.Get(), L"work", fixture.directory.wide(), 64), "setup failed");
  const ULONG id = invoke_container_number(*host.Get(), L"getContainerId", 0);
  DISPPARAMS none{};
  const auto no_args = [&](const wchar_t* name, VARIANT* result = nullptr) {
    return host->Invoke(member_id(*host.Get(), name), IID_NULL, LOCALE_INVARIANT,
        DISPATCH_METHOD, &none, result, nullptr, nullptr);
  };
  const auto status = [&] {
    VARIANT value{};
    require(no_args(L"getVolumeOperationStatus", &value) == S_OK && value.vt == VT_UI4,
            "status query failed");
    return value.ulVal;
  };
  const auto wait_done = [&] {
    for (int i = 0; work.running.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "volume worker did not finish");
  };
  // beginUnmount stays last: the checks after this loop rely on the session
  // ending it with nothing mounted.
  for (const auto* name : {L"beginUnmountContainer", L"beginMountAdoptedContainer",
                           L"beginUnmount"}) {
    fixture.volume.pause_volume.store(true);
    fixture.volume.volume_entered.store(false);
    const bool takes_no_id = std::wstring_view(name) == L"beginUnmount";
    const HRESULT started = takes_no_id
        ? no_args(name) : invoke_with_number(*host.Get(), name, id, nullptr, nullptr);
    require(started == S_OK && work.running.load() && status() == 1, "worker did not start");
    for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(fixture.volume.volume_entered.load() && fixture.volume.volume_thread != std::this_thread::get_id(),
            "volume operation ran on window thread");
    require(!work.can_cancel() && !work.request_cancel(), "volume worker accepted creation cancellation");
    require(no_args(L"getSessionState") == E_PENDING && no_args(L"unmount") == E_PENDING &&
            no_args(L"beginUnmount") == E_PENDING && no_args(L"getCreationProgress") == E_PENDING &&
            no_args(L"cancelContainerCreation") == E_PENDING, "worker allowed concurrent session access");
    fixture.volume.pause_volume.store(false);
    wait_done();
    require(status() == 2, "successful volume operation was not reported");
  }
  require(!fixture.volume.mounted_state, "unified unmount did not finish");
  require(invoke_with_number(*host.Get(), L"beginMountAdoptedContainer", id, nullptr, nullptr) == S_OK,
          "mount did not start");
  wait_done();
  require(status() == 2 && fixture.volume.mounted_state, "mount did not open the container");
  // A finished mount must never leave the forced-cleanup retry armed; that
  // eligibility belongs to a busy unmount alone.
  require(no_args(L"beginForceUnmountLast") == E_ACCESSDENIED,
          "a finished mount left forced cleanup eligible");
  require(no_args(L"beginUnmount") == S_OK, "cleanup after mount did not start");
  wait_done();
  fixture.volume.reject_unmount = true;
  require(no_args(L"beginUnmount") == S_OK, "failure worker did not start");
  wait_done();
  require(status() == 3, "unmount failure was reported as success");
  fixture.volume.reject_unmount = false;
  fixture.volume.pause_volume.store(true);
  require(invoke_with_number(*host.Get(), L"beginMountAdoptedContainer", id, nullptr, nullptr) == S_OK,
          "retry did not start");
  work.interrupted.store(true);
  fixture.volume.pause_volume.store(false);
  wait_done();
  require(status() == 10 && no_args(L"beginUnmount") == E_PENDING,
          "removed card allowed success or new work before lifecycle cleanup");
  work.interrupted.store(false);
  require(no_args(L"beginUnmount") == S_OK, "cleanup retry did not start");
  wait_done();
  require(status() == 2, "failure poisoned next operation");
  using usb_vault::volume::DriverError;
  const std::pair<std::error_code, ULONG> failures[] = {
      {usb_vault::volume::make_error_code(DriverError::volume_busy), 4},
      {std::make_error_code(std::errc::no_such_file_or_directory), 5},
      {std::make_error_code(std::errc::permission_denied), 6},
      {usb_vault::volume::make_error_code(DriverError::authentication_failed), 7},
      {usb_vault::volume::make_error_code(DriverError::read_failed), 8},
      {usb_vault::volume::make_error_code(DriverError::version_mismatch), 9},
      {std::error_code(ERROR_SHARING_VIOLATION, std::system_category()), 4},
      {std::error_code(ERROR_PATH_NOT_FOUND, std::system_category()), 5},
      {std::error_code(ERROR_ACCESS_DENIED, std::system_category()), 6},
      {std::error_code(ERROR_IO_DEVICE, std::system_category()), 8},
      {std::make_error_code(std::errc::not_supported), 3},
  };
  for (const auto& [failure, expected] : failures) {
    fixture.volume.volume_failure = failure;
    require(no_args(L"beginUnmount") == S_OK, "failure classification did not start");
    wait_done();
    const auto actual = status();
    if (actual != expected) {
      std::cerr << "classification code=" << failure.value() << " category=" << failure.category().name()
                << " expected=" << expected << " actual=" << actual << '\n';
    }
    require(actual == expected, "failure classification was lost or fabricated");
  }
  fixture.volume.volume_failure.clear();
  require(no_args(L"beginForceUnmountLast") == E_ACCESSDENIED,
          "non-busy failure allowed forced cleanup");
  fixture.volume.volume_failure = usb_vault::volume::make_error_code(DriverError::volume_busy);
  require(invoke_with_number(*host.Get(), L"beginMountAdoptedContainer", id, nullptr, nullptr) == S_OK,
          "busy mount did not start");
  wait_done();
  fixture.volume.volume_failure.clear();
  require(status() == 4 && no_args(L"beginForceUnmountLast") == E_ACCESSDENIED,
          "mount failure allowed forced unmount");
  const auto busy_cleanup = [&](bool all) {
    fixture.volume.volume_failure = usb_vault::volume::make_error_code(DriverError::volume_busy);
    const auto started = all ? no_args(L"beginUnmount")
        : invoke_with_number(*host.Get(), L"beginUnmountContainer", id, nullptr, nullptr);
    require(started == S_OK, "busy cleanup did not start");
    wait_done();
    fixture.volume.volume_failure.clear();
    require(status() == 4, "busy cleanup result lost");
    // The page refreshes both new partition fields before offering force.
    for (const auto query : {L"getContainerPartition", L"getContainerConversion"}) {
      VARIANT observed{};
      const auto hr = invoke_with_number(*host.Get(), query, 0, &observed, nullptr);
      ::VariantClear(&observed);
      require(SUCCEEDED(hr), "partition observation failed after busy unmount");
    }
  };
  busy_cleanup(false);
  require(invoke_with_number(*host.Get(), L"beginForceUnmountLast", id + 1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "forced cleanup accepted a caller-selected target");
  require(no_args(L"getSessionState") == S_OK, "observation failed");
  fixture.volume.pause_volume.store(true);
  require(no_args(L"beginForceUnmountLast") == S_OK && work.running.load(), "single forced cleanup did not start");
  require(no_args(L"beginForceUnmountLast") == E_PENDING, "duplicate forced cleanup was admitted");
  fixture.volume.pause_volume.store(false);
  wait_done();
  require(status() == 2 && fixture.volume.manual_force_calls == 1 && fixture.volume.manual_force_id == id,
          "single forced cleanup crossed target boundary");
  require(no_args(L"beginForceUnmountLast") == E_ACCESSDENIED, "completed force was replayed");
  busy_cleanup(true);
  require(no_args(L"beginForceUnmountLast") == S_OK, "unified forced cleanup did not start");
  wait_done();
  require(status() == 2 && fixture.volume.manual_force_calls == 2 && fixture.volume.manual_force_id == 0,
          "unified forced cleanup lost its scope");
  busy_cleanup(false);
  work.card_epoch.fetch_add(1);
  require(no_args(L"beginForceUnmountLast") == E_ACCESSDENIED, "card change retained stale force target");
  busy_cleanup(false);
  require(no_args(L"unmount") == S_OK && no_args(L"beginForceUnmountLast") == E_ACCESSDENIED,
          "unrelated mutation retained stale force target");
  busy_cleanup(false);
  fixture.volume.volume_failure = std::make_error_code(std::errc::io_error);
  require(no_args(L"beginForceUnmountLast") == S_OK, "failing force did not start");
  wait_done();
  fixture.volume.volume_failure.clear();
  require(status() == 8 && no_args(L"beginForceUnmountLast") == E_ACCESSDENIED,
          "force failure fabricated success or left a force grant");
  require(invoke_with_number(*host.Get(), L"beginMountAdoptedContainer", 0, nullptr, nullptr) == DISP_E_TYPEMISMATCH,
          "invalid identifier was admitted");
  require(no_args(L"beginMountAdoptedContainer") == DISP_E_BADPARAMCOUNT, "missing identifier was admitted");
  require(no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED, "unprompted force exit admitted");
  work.arm_close_confirmation();
  require(no_args(L"dismissCloseConfirmation") == S_OK &&
          no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED, "cancel retained force exit grant");
  work.arm_close_confirmation();
  work.card_epoch.fetch_add(1);
  require(no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED, "card change retained exit grant");
  work.arm_close_confirmation();
  require(no_args(L"unmount") == S_OK && no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED,
          "mutation retained exit grant");
  work.arm_close_confirmation();
  fixture.volume.mounted_state = true;
  const int forces_before_exit = fixture.volume.manual_force_calls;
  require(no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED,
          "retired page force-exit route admitted an armed confirmation");
  require(!work.running.load() && fixture.volume.mounted_state &&
          fixture.volume.manual_force_calls == forces_before_exit,
          "retired force-exit route touched the mounted volume");
}

void test_selected_batches_are_serial_bounded_and_session_scoped() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  for (const auto* name : {L"one.hc", L"two.hc", L"three.hc", L"four.hc"})
    static_cast<void>(fixture.volume.adopt(name, 64ULL * 1024 * 1024));
  for (auto& entry : fixture.volume.containers) entry.mounted = entry.id == 4;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, &g_close_sink, &host, nullptr, &work)), "batch host creation failed");
  DISPPARAMS none{};
  const auto no_args = [&](const wchar_t* name, VARIANT* result = nullptr) {
    return host->Invoke(member_id(*host.Get(), name), IID_NULL, LOCALE_INVARIANT,
        DISPATCH_METHOD, &none, result, nullptr, nullptr);
  };
  const auto begin = [&](const wchar_t* name, std::wstring_view selection) {
    VARIANT value{}; value.vt = VT_BSTR;
    value.bstrVal = ::SysAllocStringLen(selection.data(), static_cast<UINT>(selection.size()));
    require(value.bstrVal != nullptr, "batch test allocation failed");
    DISPPARAMS arguments{&value, nullptr, 1, 0};
    const auto hr = host->Invoke(member_id(*host.Get(), name), IID_NULL, LOCALE_INVARIANT,
        DISPATCH_METHOD, &arguments, nullptr, nullptr, nullptr);
    ::VariantClear(&value);
    return hr;
  };
  const auto progress = [&] {
    VARIANT value{};
    require(no_args(L"getBatchProgress", &value) == S_OK && value.vt == VT_BSTR,
            "batch progress unavailable");
    const std::wstring text(value.bstrVal, ::SysStringLen(value.bstrVal));
    ::VariantClear(&value); return text;
  };
  const auto done = [&] {
    for (int i = 0; work.running.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "batch did not finish");
  };
  for (const auto* invalid : {L"", L"0", L"01", L"1,1", L"1,", L",1", L"1,,2", L"-1", L"1 2", L"4294967296", L"1,999"})
    require(begin(L"beginMountBatch", invalid) == E_INVALIDARG, "invalid selection admitted");
  require(begin(L"beginMountBatch", std::wstring_view(L"1\0,2", 5)) == E_INVALIDARG,
          "embedded NUL admitted");
  require(begin(L"beginMountBatch", std::wstring(11265, L'1')) == E_INVALIDARG, "oversized batch admitted");
  require(no_args(L"beginMountBatch") == DISP_E_BADPARAMCOUNT &&
          invoke_with_number(*host.Get(), L"beginMountBatch", 1, nullptr, nullptr) == DISP_E_TYPEMISMATCH &&
          fixture.volume.batch_calls.empty(), "invalid request performed work");

  fixture.volume.batch_fail_id = 1;
  require(begin(L"reorderContainers", L"4,3,2,1") == S_OK && fixture.volume.saved_order == std::vector<std::uint32_t>{4,3,2,1},
          "authenticated order did not reach the session");
  for (const auto* invalid : {L"1,1", L"01,2,3,4", L"1,2,3,999", L"1,2", L""})
    require(FAILED(begin(L"reorderContainers", invalid)), "invalid order admitted");
  require(invoke_with_number(*host.Get(), L"reorderContainers", 1, nullptr, nullptr) == DISP_E_TYPEMISMATCH,
          "numeric order admitted");
  fixture.volume.pause_volume.store(true);
  fixture.volume.volume_entered.store(false);
  require(begin(L"beginMountBatch", L"3,1,2,4") == S_OK, "mount batch did not start");
  require(begin(L"reorderContainers", L"1,2,3,4") == E_PENDING, "ordering raced a volume worker");
  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(progress() == L"1,0,4,0,1,3" &&
          invoke_container_number(*host.Get(), L"getBatchItemId", 0) == 3 &&
          invoke_container_text(*host.Get(), L"getBatchItemName", 0) == L"three.hc" &&
          fixture.volume.volume_thread != std::this_thread::get_id(), "batch current item or thread is wrong");
  require(no_args(L"getSessionState") == E_PENDING && no_args(L"beginUnmount") == E_PENDING &&
          no_args(L"getVolumeOperationStatus") == E_PENDING &&
          begin(L"beginUnmountBatch", L"1") == E_PENDING && !work.request_cancel(),
          "batch admitted concurrent session work or manual cancellation");
  fixture.volume.pause_volume.store(false);
  done();
  require(progress() == L"2,0,4,4,0,0" && fixture.volume.batch_calls == std::vector<std::uint32_t>{3,1,2},
          "batch stopped on failure, reordered or repeated work");
  for (std::uint32_t i = 0; i < 4; ++i) {
    const ULONG expected[] = {2,4,2,11};
    require(invoke_container_number(*host.Get(), L"getBatchItemOutcome", i) == expected[i], "item outcome mismatch");
  }
  require(no_args(L"beginForceUnmountLast") == E_ACCESSDENIED, "batch created a force grant");
  VARIANT unused{};
  require(invoke_with_number(*host.Get(), L"getBatchItemName", 4, &unused, nullptr) == E_INVALIDARG,
          "out of range batch item admitted");
  fixture.volume.batch_calls.clear(); fixture.volume.batch_fail_id = 0;
  require(begin(L"beginMountBatch", L"1") == S_OK, "failed-item retry did not start");
  done();
  require(progress() == L"2,0,1,1,0,0" && fixture.volume.batch_calls == std::vector<std::uint32_t>{1},
          "retry reused old outcomes or included unselected items");

  fixture.volume.batch_calls.clear(); fixture.volume.batch_fail_id = 2;
  require(begin(L"beginUnmountBatch", L"1,2,3,4") == S_OK, "unmount batch did not start");
  done();
  require(progress() == L"2,1,4,4,0,0" && fixture.volume.batch_calls == std::vector<std::uint32_t>{1,2,3,4} &&
          fixture.volume.containers[1].mounted && !fixture.volume.containers[2].mounted,
          "unmount failure stopped following items or cleared failed state");
  require(invoke_container_number(*host.Get(), L"getBatchItemOutcome", 1) == 4 &&
          no_args(L"beginForceUnmountLast") == E_ACCESSDENIED, "batch busy unlocked force");
  require(begin(L"beginUnmountBatch", L"1") == S_OK, "already-unmounted batch rejected");
  done();
  require(invoke_container_number(*host.Get(), L"getBatchItemOutcome", 0) == 11, "already-unmounted item was not skipped");

  require(begin(L"beginForceUnmountBatch", L"2,999") == E_INVALIDARG &&
          no_args(L"beginForceUnmountBatch") == DISP_E_BADPARAMCOUNT &&
          invoke_with_number(*host.Get(), L"beginForceUnmountBatch", 2, nullptr, nullptr) == DISP_E_TYPEMISMATCH &&
          fixture.volume.manual_force_calls == 0, "invalid force batch performed work");
  fixture.volume.containers[2].mounted = true;
  fixture.volume.batch_calls.clear();
  require(begin(L"beginForceUnmountBatch", L"2,3,1") == S_OK, "force batch did not start");
  done();
  require(fixture.volume.manual_force_ids == std::vector<std::uint32_t>{2,3} &&
          fixture.volume.batch_calls.empty() && fixture.volume.containers[1].mounted &&
          !fixture.volume.containers[2].mounted &&
          invoke_container_number(*host.Get(), L"getBatchItemOutcome", 0) == 4 &&
          invoke_container_number(*host.Get(), L"getBatchItemOutcome", 1) == 2 &&
          invoke_container_number(*host.Get(), L"getBatchItemOutcome", 2) == 11,
          "force batch lost selection, continuation or already-unmounted semantics");
  fixture.volume.batch_fail_id = 0;
  require(begin(L"beginForceUnmountBatch", L"2") == S_OK, "force retry did not start");
  done();
  require(!fixture.volume.containers[1].mounted &&
          no_args(L"beginForceUnmountLast") == E_ACCESSDENIED, "force retry failed or granted unrelated force");

  fixture.volume.batch_calls.clear(); fixture.volume.batch_fail_id = 0;
  fixture.volume.pause_volume.store(true); fixture.volume.volume_entered.store(false);
  require(begin(L"beginMountBatch", L"1,3,4") == S_OK, "interruption batch did not start");
  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  work.card_epoch.fetch_add(1); work.interrupted.store(true);
  require(no_args(L"getBatchProgress", &unused) == E_ACCESSDENIED &&
          invoke_with_number(*host.Get(), L"getBatchItemName", 0, &unused, nullptr) == E_ACCESSDENIED,
          "removed card exposed a batch snapshot");
  fixture.volume.pause_volume.store(false); done();
  require(fixture.volume.batch_calls == std::vector<std::uint32_t>{1}, "batch continued after card removal");
  work.interrupted.store(false);
  require(no_args(L"getBatchProgress", &unused) == E_ACCESSDENIED, "old batch survived card reinsertion");
  require(begin(L"beginMountBatch", L"3") == S_OK, "new batch did not recover after lifecycle cleanup");
  done();
  require(progress() == L"2,0,1,1,0,0", "new batch inherited stale progress");
  work.arm_close_confirmation();
  require(no_args(L"beginForceUnmountForExit") == E_ACCESSDENIED && !work.running.load(),
          "completed batch reopened the retired force-exit route");
  require(no_args(L"unmount") == S_OK && no_args(L"getBatchProgress", &unused) == E_UNEXPECTED,
          "another session action retained stale batch results");
  fixture.authentication.clear_authentication();
  require(begin(L"reorderContainers", L"1,2,3,4") == E_ACCESSDENIED, "unauthenticated ordering admitted");
  require(begin(L"beginMountBatch", L"1") == E_ACCESSDENIED &&
          begin(L"beginForceUnmountBatch", L"1") == E_ACCESSDENIED &&
          begin(L"beginUnmountBatch", L"1") == E_ACCESSDENIED, "unauthenticated batch admitted");
}

void test_adopted_container_mount_and_unmount_route_through_identifiers() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(create_container_succeeds(*host.Get(), L"work",
                                    fixture.directory.wide(), 64),
          "creation was refused");
  const ULONG id = invoke_container_number(*host.Get(), L"getContainerId", 0);

  require(invoke_with_number(*host.Get(), L"unmountContainer", id, nullptr,
                             nullptr) == S_OK &&
              fixture.volume.unmount_container_calls == 1 &&
              !invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0).empty(),
          "unmounting by identifier did not clear the drive");

  require(invoke_selection(*host.Get(), L"mountAdoptedContainer", id) &&
              fixture.chooser.existing_calls == 0 &&
              invoke_container_mounted(*host.Get(), 0),
          "remounting by identifier asked for the file again");

  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"mountAdoptedContainer", id + 7,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host mounted an identifier the session never adopted");
  require(invoke_with_number(*host.Get(), L"unmountContainer", id + 7, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host unmounted an identifier the session never adopted");

  // Dropping a container off the list is the same shape of call, and the same
  // refusal for an identifier nobody adopted.
  require(invoke_with_number(*host.Get(), L"forgetContainer", id + 7, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host dropped an identifier the session never adopted");
  require(invoke_with_number(*host.Get(), L"unmountContainer", id, nullptr,
                             nullptr) == S_OK,
          "unmounting before the drop failed");
  require(invoke_with_number(*host.Get(), L"forgetContainer", id, nullptr,
                             nullptr) == S_OK &&
              // The refused attempt above reached the session too, so this is
              // the second call and the first one that changed anything.
              fixture.volume.forget_container_calls == 2 &&
              invoke_container_count(*host.Get()) == 0,
          "dropping by identifier did not take the container off the list");
}

// Builds `createContainerBMib(name, directory, outer, hidden, second,
// confirmation)` in COM's reverse order. The two password arrays stay with the
// caller so it can prove they were wiped.
void fill_container_b_arguments(std::array<VARIANTARG, 6>& values,
                                const std::wstring& name,
                                const std::wstring& directory,
                                const std::span<const std::byte> second) {
  initialize_byte_array(values[0], second);
  initialize_byte_array(values[1], second);
  values[2] = number_argument(16);
  values[3] = number_argument(64);
  values[4] = text_argument(directory);
  values[5] = text_argument(name);
}

void test_container_b_creation_consumes_six_reversed_arguments(bool background = false) {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto login = bytes("login-password");
  const auto second = bytes("second-password-value");

  std::array<VARIANTARG, 6> values{};
  fill_container_b_arguments(values, L"handover", fixture.directory.wide(),
                             second);
  DISPPARAMS parameters{values.data(), nullptr, 6, 0};

  VARIANT created{};
  ::VariantInit(&created);
  const HRESULT result =
      host->Invoke(member_id(*host.Get(), background ? L"beginCreateContainerBMib" : L"createContainerBMib"), IID_NULL,
                   LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &created,
                   nullptr, nullptr);
  require(result == S_OK && created.vt == VT_BOOL &&
              created.boolVal == VARIANT_TRUE,
          "container B creation was refused");
  static_cast<void>(::VariantClear(&created));
  if (background) {
    bool completed = false;
    for (int i = 0; i < 2000; ++i) {
      DISPPARAMS empty{};
      VARIANT progress{};
      const HRESULT read = host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
          LOCALE_INVARIANT, DISPATCH_METHOD, &empty, &progress, nullptr, nullptr);
      completed = SUCCEEDED(read) && progress.vt == VT_BSTR && std::wstring_view(progress.bstrVal).starts_with(L"2,");
      ::VariantClear(&progress);
      if (completed) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(completed, "background hidden creation did not complete");
  }
  require(fixture.chooser.directory_calls == 0 &&
              fixture.directory.holds_protected(fixture.volume.created_b_path) &&
              fixture.volume.created_b_display_name == L"handover",
          "container B was not created at the named location");
  require(fixture.entry.second_password_calls == 1 &&
              fixture.entry.existing_password_bytes == login &&
              fixture.entry.second_password_bytes == second,
          "the card record did not receive both passwords");
  require(fixture.volume.create_b_calls == 1 &&
              fixture.volume.created_outer_bytes == 64ULL * 1024ULL * 1024ULL &&
              fixture.volume.created_hidden_bytes ==
                  16ULL * 1024ULL * 1024ULL,
          "the volume transaction received the wrong sizes");
  for (std::size_t index = 0; index < 2; ++index) {
    require_array_wiped(values[index], second.size(),
                        "a container B password array survived the call");
  }
  // The location is not a password, but a container path is exactly what
  // section 8.4 keeps off this machine, so the marshalled copy does not
  // outlive the call either.
  for (std::size_t index = 4; index < 6; ++index) {
    const UINT length = ::SysStringLen(values[index].bstrVal);
    require(length != 0, "a container B location argument lost its length");
    for (UINT position = 0; position < length; ++position) {
      require(values[index].bstrVal[position] == L'\0',
              "a container B location argument survived the call");
    }
  }
  for (VARIANTARG& value : values) {
    static_cast<void>(::VariantClear(&value));
  }

  Fixture short_call;
  short_call.authentication.establish(root_secret());
  ComPtr<IDispatch> short_host = make_host(short_call.controller);
  DISPPARAMS empty{};
  require(short_host->Invoke(member_id(*short_host.Get(),
                                       L"createContainerBMib"),
                             IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                             &empty, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "container B accepted the wrong argument count");
  require(short_call.entry.second_password_calls == 0 &&
              short_call.volume.create_b_calls == 0,
          "a malformed container B call still reached the product");

  // 8.7.2: a rejected location stops this shape exactly where it stops a
  // standard one, before the card record is touched, and the passwords that
  // came with it are still wiped.
  Fixture rejected;
  rejected.authentication.establish(root_secret());
  ComPtr<IDispatch> rejected_host = make_host(rejected.controller);
  std::array<VARIANTARG, 6> rejected_values{};
  fill_container_b_arguments(rejected_values, L"handover", L"Vault", second);
  DISPPARAMS rejected_parameters{rejected_values.data(), nullptr, 6, 0};
  UINT argument_error = 99;
  require(rejected_host->Invoke(
              member_id(*rejected_host.Get(), L"createContainerBMib"),
              IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
              &rejected_parameters, nullptr, nullptr, &argument_error) ==
              DISP_E_TYPEMISMATCH,
          "container B accepted a save location the page must not name");
  require(rejected.entry.second_password_calls == 0 &&
              rejected.volume.create_b_calls == 0,
          "a rejected location still rewrote the card or created a volume");
  for (std::size_t index = 0; index < 2; ++index) {
    require_array_wiped(rejected_values[index], second.size(),
                        "a rejected container B password array survived");
  }
  for (VARIANTARG& value : rejected_values) {
    static_cast<void>(::VariantClear(&value));
  }
}

void test_container_query_failure_does_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  fixture.volume.fail_container_statuses = true;

  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"getContainerCount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &result,
                       nullptr, nullptr) == E_FAIL,
          "a failed container query reported a count anyway");
  require(invoke_with_number(*host.Get(), L"getContainerName", 0, &result,
                             nullptr) == E_FAIL,
          "a failed container query reported a name anyway");
  static_cast<void>(::VariantClear(&result));
}

void test_operation_history_is_read_only_typed_and_clearable() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_operation_count(*host.Get()) == 0,
          "an unauthenticated route exposed operation history");

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
  require(
      invoke_operation_count(*host.Get()) == 2 &&
          invoke_operation_time(*host.Get(), 1) == 1788253201000.0 &&
          invoke_operation_number(*host.Get(), L"getOperationKind", 1) == 2 &&
          invoke_operation_number(*host.Get(), L"getOperationOutcome", 1) ==
              0 &&
          invoke_container_text(*host.Get(), L"getOperationContainerName", 1) ==
              L"work.hc" &&
          invoke_container_text(*host.Get(), L"getOperationDriveLetter", 1) ==
              L"X:",
      "operation history lost a typed native fact");

  DISPPARAMS no_arguments{};
  require(host->Invoke(member_id(*host.Get(), L"clearOperationHistory"),
                       IID_NULL,
                       LOCALE_INVARIANT,
                       DISPATCH_METHOD,
                       &no_arguments,
                       nullptr,
                       nullptr,
                       nullptr) == S_OK &&
              fixture.volume.clear_operation_history_calls == 1 &&
              invoke_operation_count(*host.Get()) == 0,
          "the clear-history action did not remove the current-session facts");
}

void test_operation_history_query_failures_do_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.fail_operation_history = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(
      host->Invoke(member_id(*host.Get(), L"getOperationCount"),
                   IID_NULL,
                   LOCALE_INVARIANT,
                   DISPATCH_METHOD,
                   &parameters,
                   &result,
                   nullptr,
                   nullptr) == E_FAIL &&
          invoke_with_number(
              *host.Get(), L"getOperationKind", 0, &result, nullptr) == E_FAIL,
      "a failed history query returned a substitute fact");
  static_cast<void>(::VariantClear(&result));
}

// Every file-manager call takes its own shape, so the tests build the argument
// array themselves rather than hiding COM's reverse order behind a helper.
[[nodiscard]] HRESULT invoke_named(IDispatch& host, const wchar_t* const name,
                                   VARIANTARG* const values, const UINT count,
                                   VARIANT* const result,
                                   UINT* const argument_error) {
  DISPPARAMS parameters{values, nullptr, count, 0};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr,
                     argument_error);
}

void clear_arguments(VARIANTARG* const values, const UINT count) {
  for (UINT index = 0; index < count; ++index) {
    static_cast<void>(::VariantClear(&values[index]));
  }
}

// The activation route reuses the login surface: state 5, the card number to
// send to the issuer, and a code that is checked natively against the card.
void test_activation_bridge_gates_and_renews() {
  const auto directory = std::filesystem::temp_directory_path() /
                         (L"uv-host-activation-" + std::to_wstring(::GetCurrentProcessId()));
  std::filesystem::remove_all(directory);
  const usb_vault::licensing::ActivationSigner signer =
      usb_vault::licensing::ActivationSigner::generate();
  usb_vault::application::ActivationStoreV1 store(
      (directory / L"activation.dat").wstring(), signer.public_key());
  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume(authentication);
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller(entry, authentication, volume, policy,
                                            chooser, nullptr, &store);
  ComPtr<IDispatch> host = make_host(controller);
  const auto call = [&](const wchar_t* const name, VARIANTARG* const values,
                        const UINT count, VARIANT& result) {
    ::VariantInit(&result);
    return invoke_named(*host.Get(), name, values, count, &result, nullptr);
  };
  const auto submit = [&](const std::wstring& code, VARIANT& result) {
    VARIANTARG value;
    ::VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = ::SysAllocString(code.c_str());
    const HRESULT hr = call(L"submitActivationCode", &value, 1, result);
    ::VariantClear(&value);
    return hr;
  };

  VARIANT result;
  require(call(L"submitActivationCode", nullptr, 0, result) == DISP_E_BADPARAMCOUNT,
          "activation accepted a missing code");
  require(submit(L"X", result) == E_ACCESSDENIED, "activation accepted without a card");
  require(call(L"getActivationCardNumber", nullptr, 0, result) == S_OK &&
              result.vt == VT_BSTR && ::SysStringLen(result.bstrVal) == 0,
          "a card number was reported without a card");
  ::VariantClear(&result);

  entry.route = CardEntryRoute::login_required;
  entry.identity = 0x1A2B3C4D;
  require(invoke_state(*host.Get()) == 5L, "an unactivated card did not map to state 5");
  require(call(L"getActivationStatus", nullptr, 0, result) == S_OK && result.vt == VT_I4 &&
              result.lVal == 0, "a missing grant was not reported");
  require(call(L"getActivationCardNumber", nullptr, 0, result) == S_OK &&
              result.vt == VT_BSTR && std::wstring_view(result.bstrVal) == L"1A2B-3C4D",
          "the card number was not shown as grouped hexadecimal");
  ::VariantClear(&result);

  VARIANTARG number;
  ::VariantInit(&number);
  number.vt = VT_I4;
  require(call(L"submitActivationCode", &number, 1, result) == DISP_E_TYPEMISMATCH,
          "a non-string code was accepted");
  require(submit(L"激活", result) == S_OK && result.lVal == 1,
          "a non-ASCII code was not invalid");
  require(submit(L"  x", result) == S_OK && result.lVal == 1,
          "garbage was not invalid");
  const std::string other = signer.sign({0x1A2B3C4E, 4102444800});
  require(submit(std::wstring(other.begin(), other.end()), result) == S_OK &&
              result.lVal == 2, "another card's code was not reported as such");
  require(invoke_state(*host.Get()) == 5L, "a rejected code opened the login route");

  const std::string code = signer.sign({0x1A2B3C4D, 4102444800});
  require(submit(std::wstring(code.begin(), code.end()), result) == S_OK &&
              result.vt == VT_I4 && result.lVal == 0, "a valid code was not accepted");
  require(invoke_state(*host.Get()) == 2L, "an accepted code did not open login");
  require(call(L"getActivationStatus", nullptr, 0, result) == S_OK && result.lVal == 1,
          "an accepted grant was not active");
  require(call(L"getActivationExpiry", nullptr, 0, result) == S_OK && result.vt == VT_R8 &&
              result.dblVal == 4102444800.0, "the expiry was not reported in UTC seconds");

  entry.route = CardEntryRoute::unavailable;
  require(submit(std::wstring(code.begin(), code.end()), result) == E_ACCESSDENIED,
          "activation was accepted on an unavailable card");
  std::filesystem::remove_all(directory);
}

void test_a_decoy_survey_walks_on_a_worker_and_reports_identifiers() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, nullptr, &host, nullptr, &work)),
          "survey host creation failed");

  const auto progress = [&] {
    VARIANT value{};
    require(invoke_named(*host.Get(), L"getDecoySurveyProgress", nullptr, 0,
                         &value, nullptr) == S_OK && value.vt == VT_BSTR,
            "survey progress unavailable");
    const std::wstring text(value.bstrVal, ::SysStringLen(value.bstrVal));
    static_cast<void>(::VariantClear(&value));
    return text;
  };

  VARIANT unread{};
  require(invoke_named(*host.Get(), L"getDecoySurveyProgress", nullptr, 0,
                       &unread, nullptr) == E_UNEXPECTED,
          "a survey was readable before one ran");

  fixture.volume.survey_total = 3;
  fixture.volume.survey_result = {2, 5};
  fixture.volume.survey_unreachable = 1;
  fixture.volume.pause_volume.store(true);
  fixture.volume.volume_entered.store(false);

  ByteArray password(bytes("decoy-password"));
  VARIANT started{};
  require(invoke_named(*host.Get(), L"beginDecoySurvey", &password.value(), 1,
                       &started, nullptr) == S_OK,
          "the survey did not start");
  static_cast<void>(::VariantClear(&started));
  password.require_wiped("the decoy password survived the bridge");

  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // The worker owns the session while it walks, so only the survey's own
  // readers answer and the page cannot start anything else.
  VARIANT blocked{};
  require(invoke_named(*host.Get(), L"getSessionState", nullptr, 0, &blocked,
                       nullptr) == E_PENDING &&
              progress().starts_with(L"1,"),
          "the survey admitted unrelated work or reported no state");

  fixture.volume.pause_volume.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(!work.running.load(), "the survey did not finish");
  VARIANT refreshed{};
  require(invoke_named(*host.Get(), L"refreshDecoyCandidates", nullptr, 0, &refreshed, nullptr) == S_OK &&
              refreshed.vt == VT_BSTR && std::wstring(refreshed.bstrVal) == L"2,5",
          "candidate refresh did not expose the current authenticated list");
  static_cast<void>(::VariantClear(&refreshed));

  require(progress() == L"2,3,3,2,1,0",
          "the survey did not report what it walked and found");
  require(invoke_container_number(*host.Get(), L"getDecoySurveyId", 0) == 2 &&
              invoke_container_number(*host.Get(), L"getDecoySurveyId", 1) == 5,
          "the survey did not report the containers it found");
  VARIANT out_of_range{};
  require(invoke_with_number(*host.Get(), L"getDecoySurveyId", 2, &out_of_range,
                             nullptr) == E_INVALIDARG,
          "a container nobody found was readable");
  require(fixture.volume.survey_password.size() == 14 &&
              fixture.volume.survey_calls == 1,
          "the session did not receive the password the page supplied");
}

void test_a_decoy_survey_stops_when_asked_and_refuses_what_is_not_a_password() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, nullptr, &host, nullptr, &work)),
          "survey host creation failed");

  UINT argument_error = 0;
  VARIANT number{};
  ::VariantInit(&number);
  number.vt = VT_UI4;
  number.ulVal = 5;
  require(invoke_named(*host.Get(), L"beginDecoySurvey", nullptr, 0, nullptr,
                       nullptr) == DISP_E_BADPARAMCOUNT &&
              invoke_named(*host.Get(), L"beginDecoySurvey", &number, 1,
                           nullptr, &argument_error) == DISP_E_TYPEMISMATCH &&
              fixture.volume.survey_calls == 0,
          "a survey ran on something that is not a password");
  static_cast<void>(::VariantClear(&number));

  fixture.volume.survey_total = 4;
  fixture.volume.pause_volume.store(true);
  fixture.volume.volume_entered.store(false);
  ByteArray password(bytes("decoy-password"));
  VARIANT started{};
  require(invoke_named(*host.Get(), L"beginDecoySurvey", &password.value(), 1,
                       &started, nullptr) == S_OK,
          "the survey did not start");
  static_cast<void>(::VariantClear(&started));
  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  VARIANT cancelled{};
  require(invoke_named(*host.Get(), L"cancelDecoySurvey", nullptr, 0, &cancelled,
                       nullptr) == S_OK,
          "the survey refused to be stopped");
  static_cast<void>(::VariantClear(&cancelled));
  fixture.volume.pause_volume.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  VARIANT value{};
  require(invoke_named(*host.Get(), L"getDecoySurveyProgress", nullptr, 0,
                       &value, nullptr) == S_OK,
          "a stopped survey reported nothing");
  const std::wstring text(value.bstrVal, ::SysStringLen(value.bstrVal));
  static_cast<void>(::VariantClear(&value));
  require(text.starts_with(L"2,") && text.ends_with(L",1"),
          "a stopped survey did not say it was stopped");
}

void test_opening_maintenance_runs_on_the_volume_worker() {
  for (const int previous_mode : {0, 1, 2}) {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    usb_vault::ui::CreationWorkState work;
    ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(
                fixture.controller, nullptr, &host, nullptr, &work)),
            "maintenance host creation failed");

    // Cover a completed survey, ordinary batch, and force batch before a mount.
    if (previous_mode == 0) {
      ByteArray password(bytes("decoy-password"));
      VARIANT surveyed{};
      require(invoke_named(*host.Get(), L"beginDecoySurvey", &password.value(), 1,
                           &surveyed, nullptr) == S_OK, "preliminary survey failed");
      static_cast<void>(::VariantClear(&surveyed));
    } else {
      const auto id = fixture.volume.adopt(L"test.hc", 64ULL * 1024 * 1024);
      fixture.volume.containers.back().mounted = true;
      VARIANT selection{}; selection.vt = VT_BSTR;
      selection.bstrVal = ::SysAllocString(std::to_wstring(id).c_str());
      const auto hr = invoke_named(*host.Get(), previous_mode == 1 ? L"beginUnmountBatch" : L"beginForceUnmountBatch",
                                   &selection, 1, nullptr, nullptr);
      static_cast<void>(::VariantClear(&selection));
      require(hr == S_OK, "preliminary unmount batch failed");
    }
    for (int i = 0; work.running.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "preliminary operation did not finish");
    fixture.volume.pause_volume.store(true);
    fixture.volume.volume_entered.store(false);
    VARIANT started{};
    require(invoke_with_number(*host.Get(), L"beginOpenDecoyMaintenance", 7,
                               &started, nullptr) == S_OK,
            "opening maintenance did not start");
    static_cast<void>(::VariantClear(&started));
    for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    VARIANT progress{};
    const HRESULT polled = invoke_named(*host.Get(), L"getVolumeOperationStatus",
                                        nullptr, 0, &progress, nullptr);
    const bool running = polled == S_OK && progress.vt == VT_UI4 && progress.ulVal == 1;
    static_cast<void>(::VariantClear(&progress));
    VARIANT state{};
    const HRESULT state_read = invoke_named(*host.Get(), L"getSessionState",
                                            nullptr, 0, &state, nullptr);
    static_cast<void>(::VariantClear(&state));
    fixture.volume.pause_volume.store(false);
    for (int i = 0; work.running.load() && i < 2000; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!work.running.load(), "opening maintenance did not finish");
    require(running, "completed operation left stale mode blocking maintenance progress");
    require(state_read == E_PENDING, "mounting exposed mutable session state");
    require(invoke_named(*host.Get(), L"getVolumeOperationStatus", nullptr, 0,
                         &progress, nullptr) == S_OK && progress.vt == VT_UI4 &&
                progress.ulVal == 2, "maintenance completion was not observable");
    static_cast<void>(::VariantClear(&progress));
    require(invoke_named(*host.Get(), L"getSessionState", nullptr, 0,
                         &state, nullptr) == S_OK,
            "completed maintenance still blocked session refresh");
    static_cast<void>(::VariantClear(&state));
    require(fixture.volume.open_maintenance_calls == 1 &&
                fixture.volume.opened_maintenance_id == 7 &&
                fixture.volume.volume_thread != std::this_thread::get_id(),
            "maintenance did not open the named container off the UI thread");
    }
}

void test_maintenance_facts_are_async_current_and_session_scoped() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.maintenance_space = {
      .total_bytes = 4096ULL * 1024ULL * 1024ULL,
      .free_bytes = 1024ULL * 1024ULL * 1024ULL,
      .safe_free_bytes = 512ULL * 1024ULL * 1024ULL + 513,
  };
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, nullptr, &host, nullptr, &work)),
          "maintenance fact host creation failed");

  VARIANT result{};
  require(invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr, 0,
                       &result, nullptr) == E_UNEXPECTED,
          "capacity was readable before it was measured");

  const auto wait_done = [&] {
    for (int i = 0; work.running.load() && i < 2000; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!work.running.load(), "maintenance fact operation did not finish");
  };
  require(invoke_with_number(*host.Get(), L"beginMeasureDecoySpace", 7,
                             nullptr, nullptr) == S_OK,
          "capacity measurement did not start");
  wait_done();
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr, 0,
                       &result, nullptr) == S_OK && result.vt == VT_BSTR &&
              std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal)) ==
                  L"4294967296,1073741824,536871425",
          "capacity did not return as the three exact byte facts");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.maintenance_space_calls == 1 &&
              fixture.volume.maintenance_space_id == 7 &&
              fixture.volume.volume_thread != std::this_thread::get_id(),
          "capacity did not measure the named container off the UI thread");

  fixture.volume.fail_maintenance_space = true;
  require(invoke_with_number(*host.Get(), L"beginMeasureDecoySpace", 7,
                             nullptr, nullptr) == S_OK,
          "failing capacity refresh did not start");
  wait_done();
  require(invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr, 0,
                       &result, nullptr) == E_UNEXPECTED,
          "a failed refresh exposed the previous capacity as current");
  fixture.volume.fail_maintenance_space = false;

  fixture.volume.pause_volume.store(true);
  fixture.volume.volume_entered.store(false);
  require(invoke_with_number(*host.Get(), L"beginMeasureDecoySpace", 7,
                             nullptr, nullptr) == S_OK,
          "capacity refresh did not start");
  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr, 0,
                       &result, nullptr) == E_PENDING,
          "a refresh exposed the previous capacity as current");
  fixture.volume.pause_volume.store(false);
  wait_done();

  require(invoke_with_number(*host.Get(), L"beginEndDecoyMaintenance", 7,
                             nullptr, nullptr) == S_OK,
          "ending maintenance did not start");
  wait_done();
  require(fixture.volume.end_maintenance_calls == 1 &&
              fixture.volume.ended_maintenance_id == 7 &&
              invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr,
                           0, &result, nullptr) == E_UNEXPECTED,
          "ending maintenance retained a capacity snapshot");

  require(invoke_with_number(*host.Get(), L"beginMeasureDecoySpace", 7,
                             nullptr, nullptr) == S_OK,
          "second capacity measurement did not start");
  wait_done();
  work.card_epoch.fetch_add(1);
  work.interrupted.store(true);
  require(invoke_named(*host.Get(), L"getDecoyMaintenanceSpace", nullptr, 0,
                       &result, nullptr) == E_ACCESSDENIED,
          "a removed card exposed the previous session's capacity");
}

// 🔴 One call brings a whole directory back. Five hundred files through the
// per-entry accessors above would be two thousand serialized asynchronous
// calls, and the page would visibly stall.
void test_a_directory_listing_arrives_as_one_packed_string() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

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

  std::array<VARIANTARG, 2> values{};
  values[1] = number_argument(7);
  values[0] = text_argument(L"reports");
  VARIANT result{};
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"listContainerDirectory", values.data(), 2,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BSTR && result.bstrVal != nullptr,
          "the listing did not come back as one string");

  const std::wstring packed(result.bstrVal, ::SysStringLen(result.bstrVal));
  const std::wstring expected =
      std::wstring(L"1") + L'\x1e' + L"notes.txt" + L'\x1f' + L'0' + L'\x1f' +
      L'0' + L'\x1f' + L"1234" + L'\x1f' + L"1700000000000" + L'\x1e' +
      L"elsewhere" + L'\x1f' + L'1' + L'\x1f' + L'1' + L'\x1f' + L'0' +
      L'\x1f' + L"1700000000001";
  require(packed == expected, "the packed listing is not what the page parses");
  static_cast<void>(::VariantClear(&result));

  require(fixture.volume.listed.size() == 1 &&
              fixture.volume.listed[0].id == 7 &&
              fixture.volume.listed[0].directory == L"reports",
          "the bridge did not carry the container and the directory");
  // The directory a page is looking at is the content of an encrypted volume,
  // so the marshalled copy does not outlive the call.
  require(::SysStringLen(values[0].bstrVal) == 7 &&
              std::wstring(values[0].bstrVal, 7) == std::wstring(7, L'\0'),
          "the directory the page named outlived the call");
  clear_arguments(values.data(), 2);

  // An empty directory is still a header record, so the page can tell it from
  // a call that returned nothing.
  Fixture empty_fixture;
  empty_fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> empty_host = make_host(empty_fixture.controller);
  std::array<VARIANTARG, 2> empty_values{};
  empty_values[1] = number_argument(1);
  empty_values[0] = text_argument(L"");
  ::VariantInit(&result);
  require(invoke_named(*empty_host.Get(), L"listContainerDirectory",
                       empty_values.data(), 2, &result, nullptr) == S_OK &&
              result.vt == VT_BSTR &&
              std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal)) ==
                  L"0",
          "an empty directory did not come back as a header alone");
  static_cast<void>(::VariantClear(&result));
  clear_arguments(empty_values.data(), 2);
}

void test_a_failed_listing_says_which_way_it_failed() {
  // One catch-all left the page telling the user their folder was gone for
  // every failure, including a container that is merely unmounted. The reasons
  // a person acts on differently have to arrive separately.
  const auto reason_for = [](Fixture& fixture) {
    ComPtr<IDispatch> host = make_host(fixture.controller);
    std::array<VARIANTARG, 2> values{};
    values[1] = number_argument(7);
    values[0] = text_argument(L"reports");
    // A null result is answered before the listing is ever attempted, so the
    // call has to ask for one to reach the failure it is about to read.
    VARIANT listed{};
    ::VariantInit(&listed);
    static_cast<void>(invoke_named(*host.Get(), L"listContainerDirectory",
                                   values.data(), 2, &listed, nullptr));
    static_cast<void>(::VariantClear(&listed));
    clear_arguments(values.data(), 2);
    VARIANT reason{};
    ::VariantInit(&reason);
    require(invoke_named(*host.Get(), L"getDirectoryError", nullptr, 0, &reason,
                         nullptr) == S_OK && reason.vt == VT_UI4,
            "the bridge did not answer why the listing failed");
    const unsigned int value = reason.ulVal;
    static_cast<void>(::VariantClear(&reason));
    return value;
  };

  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_not_mounted = true;
    require(reason_for(fixture) == 1, "an unmounted container was not reported as one");
  }
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.reject_listing = true;
    require(reason_for(fixture) == 2, "a container the session does not hold was not reported as one");
  }
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_refused = true;
    require(reason_for(fixture) == 7, "a refused directory was not reported as one");
  }
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_system_error = ERROR_ACCESS_DENIED;
    require(reason_for(fixture) == 5, "a denied directory was not reported as one");
  }
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_system_error = ERROR_NOT_READY;
    require(reason_for(fixture) == 3, "a detached volume was not reported as one");
  }
  {
    // Nothing is mounted behind this fixture, so a missing path cannot be
    // blamed on the folder: the drive carrying it does not answer either.
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_system_error = ERROR_PATH_NOT_FOUND;
    require(reason_for(fixture) == 3,
            "a missing path was blamed on the folder while the volume was gone");
  }
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    fixture.volume.listing_system_error = ERROR_WRITE_PROTECT;
    require(reason_for(fixture) == 6, "an unclassified failure was not reported as I/O");
  }
  {
    // A listing that works leaves no failure behind for the page to read.
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    require(reason_for(fixture) == 0, "a successful listing left a failure behind");
  }
}

void test_the_file_manager_bridge_checks_its_shapes() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  std::array<VARIANTARG, 1> too_few{};
  too_few[0] = number_argument(1);
  require(invoke_named(*host.Get(), L"listContainerDirectory", too_few.data(),
                       1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a listing with the wrong argument count was accepted");
  clear_arguments(too_few.data(), 1);

  std::array<VARIANTARG, 2> wrong_types{};
  wrong_types[1] = text_argument(L"7");
  wrong_types[0] = text_argument(L"reports");
  UINT argument_error = 99;
  require(invoke_named(*host.Get(), L"listContainerDirectory",
                       wrong_types.data(), 2, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "a container identifier arrived as text and was accepted");
  clear_arguments(wrong_types.data(), 2);

  std::array<VARIANTARG, 5> wrong_arity{};
  for (UINT index = 0; index < 5; ++index) {
    wrong_arity[index] = number_argument(index);
  }
  require(invoke_named(*host.Get(), L"deleteContainerEntry", wrong_arity.data(),
                       5, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a delete with five arguments was accepted");
  require(invoke_named(*host.Get(), L"moveContainerEntry", wrong_arity.data(),
                       4, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a move with four arguments was accepted");
  clear_arguments(wrong_arity.data(), 5);

  require(fixture.volume.file_manager_calls() == 0,
          "a rejected call still reached the session");
}

void test_the_bridge_carries_each_file_manager_action() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  VARIANT result{};

  std::array<VARIANTARG, 3> three{};
  three[2] = number_argument(3);
  three[1] = text_argument(L"reports");
  three[0] = text_argument(L"old.txt");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"deleteContainerEntry", three.data(), 3,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE,
          "the delete did not report that it happened");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.deleted.size() == 1 &&
              fixture.volume.deleted[0].id == 3 &&
              fixture.volume.deleted[0].directory == L"reports" &&
              fixture.volume.deleted[0].name == L"old.txt",
          "the delete did not cross the bridge in order");
  clear_arguments(three.data(), 3);

  std::array<VARIANTARG, 3> make{};
  make[2] = number_argument(3);
  make[1] = text_argument(L"");
  make[0] = text_argument(L"archive");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"createContainerDirectory", make.data(), 3,
                       &result, nullptr) == S_OK,
          "the new directory did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.created_directories.size() == 1 &&
              fixture.volume.created_directories[0].name == L"archive",
          "the new directory arrived under another name");
  clear_arguments(make.data(), 3);

  std::array<VARIANTARG, 4> four{};
  four[3] = number_argument(3);
  four[2] = text_argument(L"reports");
  four[1] = text_argument(L"old.txt");
  four[0] = text_argument(L"new.txt");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"renameContainerEntry", four.data(), 4,
                       &result, nullptr) == S_OK,
          "the rename did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.renamed.size() == 1 &&
              fixture.volume.renamed[0].name == L"old.txt" &&
              fixture.volume.renamed[0].new_name == L"new.txt",
          "the rename lost one of its two names");
  clear_arguments(four.data(), 4);

  std::array<VARIANTARG, 5> five{};
  five[4] = number_argument(3);
  five[3] = text_argument(L"reports");
  five[2] = text_argument(L"q1.txt");
  five[1] = number_argument(4);
  five[0] = text_argument(L"archive");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"moveContainerEntry", five.data(), 5,
                       &result, nullptr) == S_OK,
          "the move did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.moved.size() == 1 &&
              fixture.volume.moved[0].source_id == 3 &&
              fixture.volume.moved[0].source_directory == L"reports" &&
              fixture.volume.moved[0].name == L"q1.txt" &&
              fixture.volume.moved[0].destination_id == 4 &&
              fixture.volume.moved[0].destination_directory == L"archive",
          "the move arrived with its two ends confused");
  clear_arguments(five.data(), 5);

  std::array<VARIANTARG, 5> copy{};
  copy[4] = number_argument(4);
  copy[3] = text_argument(L"archive");
  copy[2] = text_argument(L"q1.txt");
  copy[1] = number_argument(3);
  copy[0] = text_argument(L"");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"copyContainerEntry", copy.data(), 5,
                       &result, nullptr) == S_OK,
          "the copy did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.copied.size() == 1 &&
              fixture.volume.copied[0].source_id == 4 &&
              fixture.volume.copied[0].destination_id == 3 &&
              fixture.volume.copied[0].destination_directory.empty(),
          "the copy arrived with its two ends confused");
  clear_arguments(copy.data(), 5);
}

// P5.9: an encrypted export runs on the session worker. Its passwords are
// wiped at the bridge, only its status and cancel answer while it runs, and
// its outcome is a role-neutral state and code.
void test_selection_export_bridge_parses_bounds_and_wipes_inputs() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto start = [&](std::wstring_view packed, bool encrypted) {
    std::array<VARIANTARG, 6> values{};
    values[5].vt = VT_BOOL;
    values[5].boolVal = encrypted ? VARIANT_TRUE : VARIANT_FALSE;
    values[4] = text_argument(std::wstring(packed));
    values[3] = text_argument(L"D:\\Out");
    values[2] = text_argument(L"Combined");
    const auto secret = encrypted ? bytes("long enough export pass") : bytes("");
    initialize_byte_array(values[1], secret); initialize_byte_array(values[0], secret);
    VARIANT result{};
    const auto hr = invoke_named(*host.Get(), L"beginExportSelection", values.data(), 6, &result, nullptr);
    require_array_wiped(values[1], secret.size(), "selection password survived the bridge");
    require_array_wiped(values[0], secret.size(), "selection confirmation survived the bridge");
    for (std::size_t index = 2; index <= 4; ++index)
      for (UINT unit = 0; unit < ::SysStringLen(values[index].bstrVal); ++unit)
        require(values[index].bstrVal[unit] == 0, "selection text survived the bridge");
    ::VariantClear(&result); clear_arguments(values.data(), 6); return hr;
  };
  const auto settle = [&] {
    std::wstring status;
    for (int i = 0; i < 4000; ++i) {
      VARIANT value{};
      require(invoke_named(*host.Get(), L"encryptedTransferStatus", nullptr, 0, &value, nullptr) == S_OK,
          "selection status must be available");
      status.assign(value.bstrVal, ::SysStringLen(value.bstrVal)); ::VariantClear(&value);
      if (!status.starts_with(L"1,")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return status;
  };
  require(start(L"3\x1f" L"reports\x1f" L"q1.txt\x1e" L"4\x1f\x1f" L"q1.txt", true) == S_OK &&
      settle() == L"2,40,40,0,2", "grouped encrypted export must complete");
  require(fixture.volume.selected_exports.size() == 2 &&
      fixture.volume.selected_exports[0].directory == L"reports" &&
      fixture.volume.selected_exports[1].id == 4 && fixture.volume.selected_folder == L"Combined" &&
      fixture.volume.selected_destination == L"D:\\Out", "selection order and names must cross the bridge");
  require(start(L"3\x1f\x1f" L"q1.txt", false) == S_OK && settle() == L"2,40,40,0,2",
      "plaintext groups must accept empty password arrays");
  for (const auto malformed : {L"0\x1f\x1f" L"q1.txt", L"4294967296\x1f\x1f" L"q1.txt",
      L"3\x1f" L"missing", L"3\x1f\x1f", L"3\x1f\x1f" L"q1.txt\x1f"})
    require(start(malformed, true) == E_INVALIDARG, "malformed selection packing must be refused");
}

void test_an_encrypted_export_runs_on_the_worker() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, nullptr, &host, nullptr, &work)),
          "encrypted export host creation failed");
  const auto start = [&](const std::string_view password, const std::string_view confirmation) {
    std::array<VARIANTARG, 7> values{};
    values[6] = number_argument(3);
    values[5] = text_argument(L"reports");
    values[4] = text_argument(L"q1.txt");
    values[3] = text_argument(L"D:\\Out");
    values[2] = text_argument(L"q1 package");
    initialize_byte_array(values[1], bytes(password));
    initialize_byte_array(values[0], bytes(confirmation));
    VARIANT started{};
    const HRESULT result = invoke_named(*host.Get(), L"beginEncryptedExport",
                                        values.data(), 7, &started, nullptr);
    require_array_wiped(values[1], password.size(), "the export password survived the bridge");
    require_array_wiped(values[0], confirmation.size(), "the export confirmation survived the bridge");
    static_cast<void>(::VariantClear(&started));
    clear_arguments(values.data(), 7);
    return result;
  };
  const auto status = [&] {
    VARIANT value{};
    require(invoke_named(*host.Get(), L"encryptedTransferStatus", nullptr, 0, &value, nullptr) == S_OK &&
                value.vt == VT_BSTR,
            "the encrypted transfer status was unavailable");
    const std::wstring text(value.bstrVal, ::SysStringLen(value.bstrVal));
    static_cast<void>(::VariantClear(&value));
    return text;
  };
  const auto settle = [&] {
    std::wstring text = status();
    for (int i = 0; text.starts_with(L"1,") && i < 4000; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      text = status();
    }
    return text;
  };

  require(start("long enough export pass", "long enough export pazz") == S_OK,
          "a mismatched export did not start its worker");
  require(settle() == L"3,0,0,2,0", "a mismatched export password was not reported as such");
  require(start("short", "short") == S_OK && settle() == L"3,0,0,3,0",
          "a weak export password was not reported as such");
  require(fixture.volume.encrypted_exports.empty(), "a refused export reached the session");

  fixture.volume.pause_volume.store(true);
  fixture.volume.volume_entered.store(false);
  require(start("long enough export pass", "long enough export pass") == S_OK,
          "the encrypted export did not start");
  for (int i = 0; !fixture.volume.volume_entered.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  VARIANT blocked{};
  require(invoke_named(*host.Get(), L"getSessionState", nullptr, 0, &blocked, nullptr) == E_PENDING,
          "the session answered while an encrypted export owned it");
  require(status().starts_with(L"1,"), "a running export did not say so");
  VARIANT cancelled{};
  require(invoke_named(*host.Get(), L"cancelEncryptedTransfer", nullptr, 0, &cancelled, nullptr) == S_OK &&
              cancelled.vt == VT_BOOL && cancelled.boolVal == VARIANT_TRUE,
          "a running export could not be cancelled");
  fixture.volume.pause_volume.store(false);
  require(settle().starts_with(L"4,"), "a cancelled export was not reported as cancelled");
  require(fixture.volume.encrypted_exports.empty(), "a cancelled export completed anyway");

  require(start("long enough export pass", "long enough export pass") == S_OK &&
              settle() == L"2,40,40,0,2",
          "a completed export did not report its progress and success");
  require(fixture.volume.encrypted_exports.size() == 1 &&
              fixture.volume.encrypted_exports[0].id == 3 &&
              fixture.volume.encrypted_exports[0].directory == L"reports" &&
              fixture.volume.encrypted_exports[0].name == L"q1.txt" &&
              fixture.volume.encrypted_exports[0].destination == L"D:\\Out|q1 package",
          "the encrypted export crossed the bridge out of order");

  // A new card insertion retires the outcome: a later login must not learn
  // that an export happened, or how large it was.
  work.card_epoch.fetch_add(1);
  require(status() == L"0,0,0,0,0", "a finished export outlived the card insertion that ran it");

  // A mistyped argument is refused, and the passwords are still wiped.
  std::array<VARIANTARG, 7> mistyped{};
  mistyped[6] = text_argument(L"3");
  mistyped[5] = text_argument(L"reports");
  mistyped[4] = text_argument(L"q1.txt");
  mistyped[3] = text_argument(L"D:\\Out");
  mistyped[2] = text_argument(L"pkg");
  initialize_byte_array(mistyped[1], bytes("long enough export pass"));
  initialize_byte_array(mistyped[0], bytes("long enough export pass"));
  require(invoke_named(*host.Get(), L"beginEncryptedExport", mistyped.data(), 7, nullptr, nullptr) ==
              DISP_E_TYPEMISMATCH,
          "a mistyped encrypted export was accepted");
  require_array_wiped(mistyped[1], 23, "a mistyped export password survived the bridge");
  require_array_wiped(mistyped[0], 23, "a mistyped export confirmation survived the bridge");
  clear_arguments(mistyped.data(), 7);

  std::array<VARIANTARG, 2> short_call{};
  initialize_byte_array(short_call[1], bytes("long enough export pass"));
  initialize_byte_array(short_call[0], bytes("long enough export pass"));
  require(invoke_named(*host.Get(), L"beginEncryptedExport", short_call.data(), 2, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "an encrypted export with the wrong arity was accepted");
  require_array_wiped(short_call[1], 23, "a refused export password survived the bridge");
  require_array_wiped(short_call[0], 23, "a refused export confirmation survived the bridge");
  clear_arguments(short_call.data(), 2);
}

// P5.9: an import is picked natively, so the page sees a file name and never
// the path, and it runs on the worker like an export.
void test_an_encrypted_import_runs_on_the_worker() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const std::filesystem::path package = std::filesystem::path(fixture.directory.wide()) / L"family photos";
  { std::ofstream(package, std::ios::binary) << std::string(1000, 'x'); }
  fixture.chooser.existing_path = package.wstring();
  VARIANT picked{};
  require(invoke_named(*host.Get(), L"chooseImportFile", nullptr, 0, &picked, nullptr) == S_OK &&
              picked.vt == VT_BSTR &&
              std::wstring(picked.bstrVal, ::SysStringLen(picked.bstrVal)) == L"2|family photos",
          "the page was not given only the picked file name");
  static_cast<void>(::VariantClear(&picked));

  // One selection survives authentication and damaged-package failures.
  for (const unsigned int error : {1U, 10U}) {
    fixture.volume.import_failure = error;
    std::array<VARIANTARG, 3> attempt{};
    attempt[2] = number_argument(5);
    attempt[1] = text_argument(L"in");
    initialize_byte_array(attempt[0], bytes("long enough export pass"));
    require(invoke_named(*host.Get(), L"beginEncryptedImport", attempt.data(), 3, nullptr, nullptr) == S_OK,
            "an import retry needed another file selection");
    require_array_wiped(attempt[0], 23, "a retry password survived the bridge");
    clear_arguments(attempt.data(), 3);
    std::wstring status;
    for (int i = 0; i < 4000; ++i) {
      VARIANT value{};
      require(invoke_named(*host.Get(), L"encryptedTransferStatus", nullptr, 0, &value, nullptr) == S_OK,
              "the retry status was unavailable");
      status.assign(value.bstrVal, ::SysStringLen(value.bstrVal));
      static_cast<void>(::VariantClear(&value));
      if (!status.starts_with(L"1,")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(status == L"3,10,10," + std::to_wstring(error) + L",1",
            "authentication and package damage shared an error code");
  }
  fixture.volume.import_failure = 0;

  std::array<VARIANTARG, 3> values{};
  values[2] = number_argument(5);
  values[1] = text_argument(L"in");
  initialize_byte_array(values[0], bytes("long enough export pass"));
  VARIANT started{};
  require(invoke_named(*host.Get(), L"beginEncryptedImport", values.data(), 3, &started, nullptr) == S_OK,
          "the import did not start");
  require_array_wiped(values[0], 23, "the import password survived the bridge");
  clear_arguments(values.data(), 3);
  static_cast<void>(::VariantClear(&started));

  std::wstring text;
  for (int i = 0; i < 4000; ++i) {
    VARIANT value{};
    require(invoke_named(*host.Get(), L"encryptedTransferStatus", nullptr, 0, &value, nullptr) == S_OK,
            "the import status was unavailable");
    text.assign(value.bstrVal, ::SysStringLen(value.bstrVal));
    static_cast<void>(::VariantClear(&value));
    if (!text.starts_with(L"1,")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(text == L"2,10,10,0,2", "the import did not finish in its write phase");
  require(fixture.volume.imports.size() == 1 && fixture.volume.imports[0].id == 5 &&
              fixture.volume.imports[0].directory == L"in" &&
              fixture.volume.imports[0].destination == package.wstring(),
          "the import crossed the bridge out of order");
}

void test_the_import_package_rename_bridge_validates_and_reports_collisions() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const auto root = std::filesystem::path(fixture.directory.wide());
  const auto original = root / L"original package";
  { std::ofstream(original, std::ios::binary) << "ciphertext";
    std::ofstream(root / L"existing package", std::ios::binary) << "keep"; }
  fixture.chooser.existing_path = original.wstring();
  static_cast<void>(fixture.controller.choose_import_file());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto rename = [&](const std::wstring_view name) {
    VARIANTARG argument = text_argument(std::wstring(name));
    VARIANT result{};
    const HRESULT hr = invoke_named(*host.Get(), L"renameImportPackage", &argument, 1, &result, nullptr);
    require(hr == S_OK && result.vt == VT_UI4, "renaming did not return a bounded status");
    const unsigned long code = result.ulVal;
    static_cast<void>(::VariantClear(&argument));
    static_cast<void>(::VariantClear(&result));
    return code;
  };
  require(rename(L"..\\escape") == 8, "the naming bridge accepted a name a volume cannot hold");
  // Naming the import never renames the package, so a name beside it is fine.
  require(rename(L"existing package") == 0 && rename(L"renamed package") == 0 &&
              fixture.controller.selected_import_package() == original.wstring() &&
              std::filesystem::exists(original) && !std::filesystem::exists(root / L"renamed package"),
          "the naming bridge renamed or moved the package file");
  fixture.authentication.clear_authentication();
  require(rename(L"later login") == 7, "a stale login renamed a package");
}

// P5.9: an import into a new container needs a fresh pick to verify and a
// verified package to create from; without them nothing starts.
void test_an_import_into_a_new_container_needs_a_verified_package() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  std::array<VARIANTARG, 1> verify{};
  initialize_byte_array(verify[0], bytes("long enough export pass"));
  require(invoke_named(*host.Get(), L"beginVerifyImport", verify.data(), 1, nullptr, nullptr) ==
              E_ACCESSDENIED,
          "a verification started without a picked package");
  require_array_wiped(verify[0], 23, "a refused verification password survived the bridge");
  clear_arguments(verify.data(), 1);

  VARIANT minimum{};
  require(invoke_named(*host.Get(), L"getVerifiedImportMinimumMib", nullptr, 0, &minimum, nullptr) == S_OK &&
              minimum.vt == VT_UI4 && minimum.ulVal == 0,
          "a minimum size was given without a verified package");

  std::array<VARIANTARG, 3> plain{};
  plain[2] = text_argument(L"restored");
  plain[1] = text_argument(L"D:\\Vaults");
  plain[0] = number_argument(64);
  require(invoke_named(*host.Get(), L"beginImportIntoNewContainerMib", plain.data(), 3, nullptr, nullptr) ==
              E_ACCESSDENIED,
          "a container was created for an import that was never verified");
  clear_arguments(plain.data(), 3);

  std::array<VARIANTARG, 6> hidden{};
  hidden[5] = text_argument(L"restored");
  hidden[4] = text_argument(L"D:\\Vaults");
  hidden[3] = number_argument(128);
  hidden[2] = number_argument(64);
  initialize_byte_array(hidden[1], bytes("second password 123"));
  initialize_byte_array(hidden[0], bytes("second password 123"));
  require(invoke_named(*host.Get(), L"beginImportIntoNewContainerBMib", hidden.data(), 6, nullptr, nullptr) ==
              E_ACCESSDENIED,
          "a hidden container was created for an import that was never verified");
  require_array_wiped(hidden[1], 19, "a refused second password survived the bridge");
  require_array_wiped(hidden[0], 19, "a refused second confirmation survived the bridge");
  clear_arguments(hidden.data(), 6);
  require(fixture.volume.created_path.empty(), "a refused import still created a container");
}

// The two exports cross the bridge like everything else: the container by
// identifier, and the location as text the session checks again.
void test_the_bridge_carries_both_exports() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  VARIANT result{};

  std::array<VARIANTARG, 4> entry{};
  entry[3] = number_argument(3);
  entry[2] = text_argument(L"reports");
  entry[1] = text_argument(L"q1.txt");
  entry[0] = text_argument(L"D:\\Out");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"exportContainerEntry", entry.data(), 4,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE,
          "the plaintext export did not report that it happened");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.exported_entries.size() == 1 &&
              fixture.volume.exported_entries[0].id == 3 &&
              fixture.volume.exported_entries[0].directory == L"reports" &&
              fixture.volume.exported_entries[0].name == L"q1.txt" &&
              fixture.volume.exported_entries[0].destination == L"D:\\Out",
          "the plaintext export crossed the bridge out of order");
  // The location is a path on this machine and the name is the content of an
  // encrypted volume, so neither outlives the call.
  require(::SysStringLen(entry[0].bstrVal) == 6 &&
              std::wstring(entry[0].bstrVal, 6) == std::wstring(6, L'\0'),
          "the export location outlived the call");
  clear_arguments(entry.data(), 4);

  std::array<VARIANTARG, 3> file{};
  file[2] = number_argument(4);
  file[1] = text_argument(L"D:\\Migration");
  file[0] = text_argument(L"工作资料");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"exportContainerFile", file.data(), 3,
                       &result, nullptr) == S_OK,
          "the migration export did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.exported_files.size() == 1 &&
              fixture.volume.exported_files[0].id == 4 &&
              fixture.volume.exported_files[0].name == L"工作资料" &&
              fixture.volume.exported_files[0].destination == L"D:\\Migration",
          "the migration export crossed the bridge out of order");
  clear_arguments(file.data(), 3);
}

void test_the_export_bridge_checks_its_shapes() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  std::array<VARIANTARG, 4> values{};
  values[3] = number_argument(3);
  values[2] = text_argument(L"reports");
  values[1] = text_argument(L"q1.txt");
  values[0] = text_argument(L"D:\\Out");
  require(invoke_named(*host.Get(), L"exportContainerEntry", values.data(), 3,
                       nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a plaintext export with three arguments was accepted");
  require(invoke_named(*host.Get(), L"exportContainerFile", values.data(), 4,
                       nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a migration export with four arguments was accepted");
  clear_arguments(values.data(), 4);

  std::array<VARIANTARG, 3> wrong_types{};
  wrong_types[2] = text_argument(L"4");
  wrong_types[1] = text_argument(L"D:\\Migration");
  wrong_types[0] = text_argument(L"");
  UINT argument_error = 99;
  require(invoke_named(*host.Get(), L"exportContainerFile", wrong_types.data(),
                       3, nullptr, &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "a container identifier arrived as text and was accepted");
  require(invoke_named(*host.Get(), L"exportContainerFile", wrong_types.data(),
                       2, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a migration export without its folder name was accepted");
  clear_arguments(wrong_types.data(), 3);

  require(fixture.volume.exported_entries.empty() &&
              fixture.volume.exported_files.empty(),
          "a rejected export still reached the session");
}

// Everything the window would do about a shell drop, recorded instead.
class RecordingDropSink final : public usb_vault::ui::IUiExternalDropTargetSink {
 public:
  bool set_target(
      const std::uint32_t id,
      const std::wstring_view relative_directory) noexcept override {
    ++set_calls;
    armed_id = id;
    armed_directory.assign(relative_directory);
    return accepts;
  }
  void clear_target() noexcept override {
    ++clear_calls;
    armed_id = 0;
    armed_directory.clear();
  }
  bool request_cancel() noexcept override {
    ++cancel_calls;
    return running;
  }
  usb_vault::ui::ExternalDropStatus status() const noexcept override {
    return reported;
  }

  bool select_files() noexcept override { ++select_calls; return armed_id != 0; }
  void show_capacity_warning(bool protection) noexcept override {
    ++warning_calls;
    last_protection = protection;
  }
  int warning_calls{};
  bool last_protection{};
  unsigned int file_progress() const noexcept override { return 142; }
  bool set_drop_bounds(int, int, int, int) noexcept override { ++bounds_calls; return true; }
  int bounds_calls{};
  int select_calls{};
  int set_calls{};
  int clear_calls{};
  int cancel_calls{};
  bool accepts{true};
  bool running{true};
  std::uint32_t armed_id{};
  std::wstring armed_directory;
  usb_vault::ui::ExternalDropStatus reported{};
};

[[nodiscard]] HRESULT invoke_set_drop_target(IDispatch& host,
                                             const std::uint32_t id,
                                             const std::wstring& directory,
                                             UINT* const argument_error) {
  // COM delivers the arguments in reverse order, as everywhere else here.
  std::array<VARIANTARG, 2> values{};
  values[1] = number_argument(id);
  values[0] = text_argument(directory);
  DISPPARAMS parameters{values.data(), nullptr, 2, 0};
  const HRESULT result =
      host.Invoke(member_id(host, L"setExternalDropTarget"), IID_NULL,
                  LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                  nullptr, argument_error);
  static_cast<void>(::VariantClear(&values[0]));
  return result;
}

[[nodiscard]] HRESULT invoke_no_arguments(IDispatch& host,
                                          const wchar_t* const name,
                                          VARIANT* const result) {
  DISPPARAMS parameters{};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr, nullptr);
}

void test_the_page_names_a_drop_destination_and_reads_only_numbers() {
  Fixture fixture;
  RecordingDropSink sink;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, &g_close_sink, &host, nullptr, &work, &sink)),
          "drop host creation failed");

  // Off the authenticated route the window cannot be armed at all, and the
  // attempt disarms whatever the page had armed before.
  fixture.entry.route = CardEntryRoute::login_required;
  require(invoke_set_drop_target(*host.Get(), 4, L"photos", nullptr) ==
                  E_ACCESSDENIED &&
              sink.set_calls == 0 && sink.clear_calls == 1,
          "an unauthenticated page armed the window for a drop");

  VARIANT selected{};
  require(invoke_no_arguments(*host.Get(),L"selectExternalFiles",&selected)==E_ACCESSDENIED&&sink.select_calls==0,"unauthenticated picker reached native host");
  fixture.authentication.establish(root_secret());
  require(invoke_set_drop_target(*host.Get(), 4, L"photos/2011", nullptr) ==
                  S_OK &&
              sink.set_calls == 1 && sink.armed_id == 4 &&
              sink.armed_directory == L"photos/2011",
          "the destination did not reach the window as the page named it");

  require(invoke_no_arguments(*host.Get(),L"selectExternalFiles",&selected)==S_OK&&selected.vt==VT_BOOL&&selected.boolVal==VARIANT_TRUE&&sink.select_calls==1,"file picker did not reach armed native host");
  static_cast<void>(::VariantClear(&selected));
  work.running.store(true);
  VARIANT bounds[4]{};
  for (auto& value : bounds) { value.vt=VT_UI4; value.ulVal=0; }
  require(invoke_named(*host.Get(),L"setExternalDropBounds",bounds,4,&selected,nullptr)==S_OK && sink.bounds_calls==1,
          "busy worker blocked hiding the native drop region");
  bounds[0].ulVal=32769;
  require(invoke_named(*host.Get(),L"setExternalDropBounds",bounds,4,&selected,nullptr)==DISP_E_TYPEMISMATCH && sink.bounds_calls==1,
          "unbounded drop rectangle reached the window");

  require(FAILED(invoke_no_arguments(*host.Get(),L"selectExternalFiles",&selected))&&sink.select_calls==1,"picker opened during active work");
  work.running.store(false);

  // A container identifier of zero names nothing, and neither half may arrive
  // as the wrong kind of value.
  UINT argument_error = 99;
  require(invoke_set_drop_target(*host.Get(), 0, L"", &argument_error) ==
                  DISP_E_TYPEMISMATCH,
          "the window was armed for a container nothing answers to");
  std::array<VARIANTARG, 2> swapped{};
  swapped[1] = text_argument(L"4");
  swapped[0] = text_argument(L"photos");
  DISPPARAMS swapped_parameters{swapped.data(), nullptr, 2, 0};
  require(host->Invoke(member_id(*host.Get(), L"setExternalDropTarget"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &swapped_parameters, nullptr, nullptr,
                       nullptr) == DISP_E_TYPEMISMATCH,
          "a textual container identifier was accepted");
  static_cast<void>(::VariantClear(&swapped[0]));
  static_cast<void>(::VariantClear(&swapped[1]));
  require(invoke_with_number(*host.Get(), L"setExternalDropTarget", 4, nullptr,
                             nullptr) == DISP_E_BADPARAMCOUNT,
          "a half-named destination was accepted");
  require(sink.set_calls == 1,
          "a refused destination still reached the window");

  require(invoke_no_arguments(*host.Get(), L"clearExternalDropTarget",
                              nullptr) == S_OK &&
              sink.clear_calls == 2,
          "leaving maintenance did not disarm the window");

  // What comes back is four numbers and nothing else: no name, no path.
  sink.reported = {.state = 1, .total = 7, .completed = 3, .failed = 1};
  VARIANT status{};
  require(invoke_no_arguments(*host.Get(), L"getExternalDropStatus", &status) ==
                  S_OK &&
              status.vt == VT_BSTR && status.bstrVal != nullptr &&
              std::wstring(status.bstrVal) == L"1,7,3,1",
          "the drop status did not arrive as four packed numbers");
  static_cast<void>(::VariantClear(&status));

  VARIANT cancelled{};
  require(invoke_no_arguments(*host.Get(), L"cancelExternalDrop", &cancelled) ==
                  S_OK &&
              cancelled.vt == VT_BOOL && cancelled.boolVal == VARIANT_TRUE &&
              sink.cancel_calls == 1,
          "the page could not call off a running drop");
  sink.running = false;
  static_cast<void>(::VariantClear(&cancelled));
  require(invoke_no_arguments(*host.Get(), L"cancelExternalDrop", &cancelled) ==
                  S_OK &&
              cancelled.boolVal == VARIANT_FALSE,
          "a drop that was not running reported itself cancelled");
  static_cast<void>(::VariantClear(&cancelled));

  // A window with no drop target of its own answers the same way every time.
  ComPtr<IDispatch> bare = make_host(fixture.controller);
  require(invoke_set_drop_target(*bare.Get(), 4, L"", nullptr) ==
                  E_ACCESSDENIED,
          "a window with no drop target claimed to be armed");
  VARIANT bare_status{};
  require(invoke_no_arguments(*bare.Get(), L"getExternalDropStatus",
                              &bare_status) == E_ACCESSDENIED,
          "a window with no drop target invented a status");
}

void test_a_running_drop_answers_only_about_itself() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  RecordingDropSink sink;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              fixture.controller, &g_close_sink, &host, nullptr, &work, &sink)),
          "drop host creation failed");

  // A drop claims the session the way a volume operation does. While it has
  // it, the page may read its progress and call it off, and nothing else.
  work.volume_operation.store(true);
  work.running.store(true);
  sink.reported = {.state = 1, .total = 2, .completed = 0, .failed = 0};

  VARIANT status{};
  require(invoke_no_arguments(*host.Get(), L"getExternalDropStatus", &status) ==
                  S_OK &&
              status.vt == VT_BSTR &&
              std::wstring(status.bstrVal) == L"1,2,0,0",
          "a running drop would not report its own progress");
  static_cast<void>(::VariantClear(&status));
  VARIANT percent{};
  require(invoke_no_arguments(*host.Get(),L"getExternalFileProgress",&percent)==S_OK && percent.vt==VT_UI4 && percent.ulVal==99,"active file progress was blocked or unbounded");
  static_cast<void>(::VariantClear(&percent));
  VARIANT cancelled{};
  require(invoke_no_arguments(*host.Get(), L"cancelExternalDrop", &cancelled) ==
                  S_OK,
          "a running drop could not be called off");
  static_cast<void>(::VariantClear(&cancelled));

  require(invoke_no_arguments(*host.Get(), L"getContainerCount", nullptr) ==
                  E_PENDING,
          "a running drop let the page reach the session");
  require(invoke_set_drop_target(*host.Get(), 5, L"", nullptr) == E_PENDING,
          "a running drop let the page move its own destination");

  // The same two members answer while a creation owns the session instead, so
  // a page polling a drop never has to know what else is running.
  work.running.store(false);
  work.volume_operation.store(false);
  work.running.store(true);
  VARIANT during_creation{};
  require(invoke_no_arguments(*host.Get(), L"getExternalDropStatus",
                              &during_creation) == S_OK,
          "the drop status was hidden behind an unrelated worker");
  static_cast<void>(::VariantClear(&during_creation));
  work.running.store(false);
}

void test_protection_query_is_boolean_scoped_and_serialized() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  RecordingDropSink sink;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, &sink)), "host failed");
  VARIANT result{};
  const auto query = [&](unsigned int id) { return invoke_with_number(*host.Get(), L"getDecoyMaintenanceWriteBlocked", id, &result, nullptr); };
  require(FAILED(query(7)), "unauthenticated status leaked");
  fixture.authentication.establish(root_secret());
  require(SUCCEEDED(query(7)) && result.vt == VT_BOOL && result.boolVal == VARIANT_FALSE, "normal state not boolean");
  fixture.volume.maintenance_blocked = true;
  require(SUCCEEDED(query(7)) && result.boolVal == VARIANT_TRUE, "blocked state not projected");
  require(sink.warning_calls == 1 && sink.last_protection, "native protection warning missing");
  require(SUCCEEDED(query(7)) && sink.warning_calls == 1, "protection polling repeated native warning");
  require(FAILED(query(0)) && FAILED(query(8)), "invalid container accepted");
  work.running.store(true);
  work.volume_operation.store(true);
  require(query(7) == E_PENDING, "query raced worker session ownership");
  work.running.store(false);
  work.interrupted.store(true);
  require(FAILED(query(7)), "interrupted card state leaked");
  require(sink.warning_calls == 1, "refused query raised a warning");
  work.interrupted.store(false);
  work.card_epoch.fetch_add(1);
  require(SUCCEEDED(query(7)) && sink.warning_calls == 2, "new session inherited warning latch");
}

void test_import_capacity_warning_rejects_stale_events_and_shares_protection_latch() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  work.card_epoch.store(42);
  RecordingDropSink sink;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, &sink)), "host failed");
  const auto poll = [&] {
    DISPPARAMS parameters{};
    VARIANT result{};
    const HRESULT hr = host->Invoke(member_id(*host.Get(), L"getExternalDropStatus"),
        IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &result, nullptr, nullptr);
    require(SUCCEEDED(hr) && result.vt == VT_BSTR && std::wstring(result.bstrVal) == L"3,2,0,1",
            "native metadata changed page status protocol");
    ::VariantClear(&result);
  };
  sink.reported = {.state=3, .total=2, .failed=1, .capacity_limited=true,
                   .container_id=7, .card_epoch=42, .run_id=1};
  poll();
  require(sink.warning_calls == 0, "unauthenticated import warned");
  fixture.authentication.establish(root_secret());
  work.running.store(true);
  work.volume_operation.store(true);
  poll();
  require(sink.warning_calls == 0, "warning queried a worker-owned session");
  work.running.store(false);
  sink.reported.card_epoch = 41;
  poll();
  sink.reported.card_epoch = 42;
  sink.reported.container_id = 8;
  poll();
  sink.reported.container_id = 7;
  poll();
  require(sink.warning_calls == 0, "stale target event was replayed");
  sink.reported.run_id = 2;
  sink.reported.capacity_limited = false;
  poll();
  require(sink.warning_calls == 0, "generic failure became capacity warning");
  sink.reported.capacity_limited = true;
  work.interrupted.store(true);
  poll();
  require(sink.warning_calls == 0, "interrupted session warned");
  work.interrupted.store(false);
  poll(); poll();
  require(sink.warning_calls == 1 && !sink.last_protection, "capacity warning missing or repeated");
  fixture.volume.maintenance_blocked = true;
  VARIANT result{};
  require(SUCCEEDED(invoke_with_number(*host.Get(), L"getDecoyMaintenanceWriteBlocked", 7, &result, nullptr)) &&
              sink.warning_calls == 1, "protection path repeated import warning");
  sink.reported.run_id = 3; // Completed failure not yet polled by the page.
  require(SUCCEEDED(invoke_with_number(*host.Get(), L"beginOpenDecoyMaintenance", 7, nullptr, nullptr)), "reopen failed");
  for (int spin=0; spin<5000 && work.running.load(); ++spin)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(!work.running.load(), "reopen did not finish");
  poll();
  require(sink.warning_calls == 1, "reopen replayed old import failure");
  require(SUCCEEDED(invoke_with_number(*host.Get(), L"getDecoyMaintenanceWriteBlocked", 7, &result, nullptr)) &&
              sink.warning_calls == 2, "reopen did not rearm protection warning");
}

class FakePartitionServices final : public usb_vault::ui::IUiPartitionServices {
 public:
  std::atomic<unsigned int> bitlocker_state{0};
  std::atomic<unsigned int> decrypt_calls{0};
  usb_vault::volume::PartitionBitLockerStatus inspect_bitlocker(const usb_vault::volume::PartitionCandidate&) override {
    const auto state = bitlocker_state.load(); return {state, state == 1 ? 100u : state == 3 ? 60u : 0u};
  }
  void decrypt_bitlocker(const usb_vault::volume::PartitionCandidate& target, const std::function<bool()>& confirm, const std::function<bool()>& cancelled) override {
    require(target.device_id[0] == 9 && confirm() && !cancelled(), "invalid decrypt consent or identity");
    bitlocker_state.store(3); decrypt_calls.fetch_add(1);
  }
  bool open_bitlocker_help() noexcept override { return true; }
  std::atomic<bool> delay{false};
  bool fail{};
  bool tool_success{true};
  int tool_calls{};
  bool changed{};
  bool gpt{};
  bool unknown_layout{};
  bool device_replaced{};
  bool check_fails{};
  std::atomic<bool> check_delay{false};
  int check_calls{};
  bool missing{};
  bool duplicate{};
  bool external_item{};
  bool ntfs_item{true};
  usb_vault::ui::CreationWorkState* expire_on_read{};
  std::vector<usb_vault::volume::PartitionCandidate> enumerate() override {
    while (delay.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (fail) throw std::runtime_error("scan failure");
    usb_vault::volume::PartitionCandidate item{};
    item.volume_path = L"private-native-identity";
    item.partition_number = 3; item.length_bytes = 1073741824;
    item.offset_bytes = 1048576; item.logical_sector_bytes = 512;
    item.layout = gpt ? usb_vault::volume::PartitionLayout::gpt : usb_vault::volume::PartitionLayout::mbr;
    if (unknown_layout) item.layout = usb_vault::volume::PartitionLayout::unknown;
    if (gpt) item.partition_id[0] = 42;
    item.device_id[0] = 9;
    if (changed) ++item.filesystem_serial;
    if (device_replaced) ++item.device_id[0];
    if (expire_on_read) expire_on_read->card_epoch.fetch_add(1);
    item.availability = usb_vault::volume::PartitionAvailability::candidate;
    item.external = external_item;
    item.ntfs = ntfs_item;
    if (missing) return {};
    if (duplicate) return {item, item};
    auto blocked = item;
    blocked.external = false;
    blocked.volume_path = L"other-private-identity";
    blocked.availability = usb_vault::volume::PartitionAvailability::windows_volume;
    return {item, blocked};
  }
  void check(const usb_vault::volume::PartitionCandidate& candidate) override {
    ++check_calls;
    require(candidate.device_id[0] == 9 && candidate.partition_number == 3, "wrong target sent to helper service");
    while (check_delay.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (check_fails) throw std::runtime_error("helper rejected");
  }
  std::wstring display_name(const usb_vault::volume::PartitionCandidate&) override { return L"T: · Test volume"; }
  bool open_disk_management() noexcept override { ++tool_calls; return tool_success; }
  std::vector<wchar_t> opened_drives;
  int this_pc_calls{};
  bool open_drive_in_explorer(const wchar_t letter) noexcept override { opened_drives.push_back(letter); return tool_success; }
  bool open_this_pc() noexcept override { ++this_pc_calls; return tool_success; }
  std::atomic<int> drive_refreshes{0};
  void refresh_drive_list() noexcept override { ++drive_refreshes; }
};

// Returning to login clears the session before the old page lets go of its
// bridge; a retired bridge must not reach the controller again.
void test_retired_host_never_reaches_its_controller() {
  {
    Fixture fixture;
    fixture.authentication.establish(root_secret());
    ComPtr<IDispatch> host = make_host(fixture.controller);
    usb_vault::ui::retire_card_product_webview_host_object(host.Get());
    usb_vault::ui::retire_card_product_webview_host_object(host.Get());
    VARIANT result{};
    require(fixture.entry.cancel_calls == 1 &&
                invoke_no_arguments(*host.Get(), L"getSessionState", &result) == E_ACCESSDENIED,
            "retired bridge did not settle once or still served calls");
    host.Reset();
    require(fixture.entry.cancel_calls == 1, "retired bridge reached the controller on release");
  }
  auto fixture = std::make_unique<Fixture>();
  ComPtr<IDispatch> host = make_host(fixture->controller);
  usb_vault::ui::retire_card_product_webview_host_object(host.Get());
  fixture.reset();  // the session goes first, as on the way back to login
  host.Reset();
  usb_vault::ui::retire_card_product_webview_host_object(nullptr);
}

void test_unmount_batch_refreshes_this_pc_once() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  for (const auto* name : {L"one.hc", L"two.hc", L"three.hc"})
    static_cast<void>(fixture.volume.adopt(name, 64ULL * 1024 * 1024));
  for (auto& entry : fixture.volume.containers) entry.mounted = true;
  usb_vault::ui::CreationWorkState work;
  FakePartitionServices services;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, &g_close_sink, &host, nullptr, &work, nullptr, &services)),
          "refresh host failed");
  const auto run = [&](const wchar_t* name, const wchar_t* selection) {
    VARIANT value{}; value.vt = VT_BSTR; value.bstrVal = ::SysAllocString(selection);
    DISPPARAMS arguments{&value, nullptr, 1, 0};
    const HRESULT hr = host->Invoke(member_id(*host.Get(), name), IID_NULL, LOCALE_INVARIANT,
        DISPATCH_METHOD, &arguments, nullptr, nullptr, nullptr);
    ::VariantClear(&value);
    require(hr == S_OK, "refresh batch did not start");
    for (int i = 0; work.running.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "refresh batch did not finish");
  };
  run(L"beginUnmountBatch", L"1,2,3");
  require(services.drive_refreshes.load() == 1 && !fixture.volume.containers[0].mounted &&
          !fixture.volume.containers[2].mounted, "unmount batch refreshed This PC other than once at the end");
  run(L"beginMountBatch", L"1,2");
  require(services.drive_refreshes.load() == 1, "mount batch refreshed This PC");
}

void test_explorer_opens_only_a_mounted_session_container() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  FakePartitionServices services;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, &g_close_sink, &host, nullptr, &work, nullptr, &services)), "explorer host failed");
  VARIANT result{};
  const auto open = [&](unsigned int id) { ::VariantClear(&result); return invoke_with_number(*host.Get(), L"openContainerDrive", id, &result, nullptr); };
  const auto this_pc = [&] { ::VariantClear(&result); return invoke_no_arguments(*host.Get(), L"openThisPc", &result); };
  const auto opened = [&] { return result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE; };
  require(open(1) == E_ACCESSDENIED && this_pc() == E_ACCESSDENIED && services.opened_drives.empty() && services.this_pc_calls == 0,
          "explorer opened without authentication");
  fixture.authentication.establish(root_secret());
  require(create_container_succeeds(*host.Get(), L"work", fixture.directory.wide(), 64), "explorer setup failed");
  const ULONG id = invoke_container_number(*host.Get(), L"getContainerId", 0);
  require(SUCCEEDED(open(id)) && opened() && services.opened_drives == std::vector<wchar_t>{L'D'}, "mounted container drive not opened");
  require(open(0) == DISP_E_TYPEMISMATCH && SUCCEEDED(open(id + 1000)) && !opened() && services.opened_drives.size() == 1,
          "unknown container reached explorer");
  services.tool_success = false;
  require(SUCCEEDED(open(id)) && !opened() && SUCCEEDED(this_pc()) && !opened(), "failed launch reported as opened");
  services.tool_success = true;
  require(SUCCEEDED(this_pc()) && opened() && services.this_pc_calls == 2, "this PC not opened");
  fixture.volume.containers[0].mounted = false;
  require(SUCCEEDED(open(id)) && !opened() && services.opened_drives.size() == 2, "unmounted container reached explorer");
  ::VariantClear(&result);
}

void test_partition_selection_lifecycle() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  FakePartitionServices services;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, nullptr, &services)), "partition host failed");
  VARIANT result{};
  const auto call = [&](const wchar_t* name) { ::VariantClear(&result); return invoke_no_arguments(*host.Get(), name, &result); };
  const auto select = [&](unsigned int id) { ::VariantClear(&result); return invoke_with_number(*host.Get(), L"selectPartition", id, &result, nullptr); };
  const auto wait = [&] {
    for (int i = 0; work.running.load() && i < 3000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "partition timeout");
  };
  require(call(L"beginPartitionRefresh") == E_ACCESSDENIED && call(L"openDiskManagement") == E_ACCESSDENIED && services.tool_calls == 0, "partition unauthenticated access");
  fixture.authentication.establish(root_secret());
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "scan start"); wait();
  require(SUCCEEDED(call(L"getPartitionList")) && result.vt == VT_BSTR && std::wstring(result.bstrVal) == L"1,0,3,1073741824,0\n2,0,3,1073741824,1", "partition projection leaked identity or incorrect facts");
  ::VariantClear(&result);
  require(SUCCEEDED(invoke_with_number(*host.Get(), L"getPartitionDisplay", 1, &result, nullptr)) &&
      result.vt == VT_BSTR && std::wstring(result.bstrVal) == L"T: · Test volume", "partition display missing");
  ::VariantClear(&result);
  require(invoke_with_number(*host.Get(), L"getPartitionDisplay", 999, &result, nullptr) == E_ACCESSDENIED,
      "unknown display ID accepted");
  require(select(2) == E_ACCESSDENIED && select(0) == DISP_E_TYPEMISMATCH && select(999) == E_ACCESSDENIED, "invalid partition selected");
  require(SUCCEEDED(select(1)) && SUCCEEDED(call(L"getSelectedPartition")) && result.ulVal == 1, "selection missing");
  require(SUCCEEDED(call(L"beginPartitionCheck")), "check start"); wait();
  require(SUCCEEDED(call(L"getPartitionCheckStatus")) && result.ulVal == 2 && services.check_calls == 1, "successful check not reported");
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "refresh start"); wait();
  require(select(1) == E_ACCESSDENIED && SUCCEEDED(call(L"getSelectedPartition")) && result.ulVal == 0 && SUCCEEDED(select(3)), "stale selection reused");
  services.tool_success = false;
  require(call(L"openDiskManagement") == E_FAIL && FAILED(select(3)), "failed launcher retained selection");
  services.tool_success = true;
  require(SUCCEEDED(call(L"openDiskManagement")) && services.tool_calls == 2, "launcher routing");
  services.gpt = true;
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "GPT scan start"); wait();
  require(SUCCEEDED(call(L"getPartitionList")) && std::wstring(result.bstrVal).find(L",1073741824,0") != std::wstring::npos,
      "supported GPT shown as unavailable");
  const auto gpt_id = static_cast<unsigned int>(std::stoul(result.bstrVal));
  require(SUCCEEDED(select(gpt_id)), "supported GPT selection rejected");
  services.gpt = false;
  services.unknown_layout = true;
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "unknown layout scan start"); wait();
  require(SUCCEEDED(call(L"getPartitionList")) && std::wstring(result.bstrVal).find(L",1073741824,3") != std::wstring::npos,
      "unknown partition layout shown as selectable");
  const auto unknown_id = static_cast<unsigned int>(std::stoul(result.bstrVal));
  require(select(unknown_id) == E_ACCESSDENIED, "unknown partition layout selected");
  services.unknown_layout = false;
  services.fail = true;
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "failed scan start"); wait();
  require(SUCCEEDED(call(L"getPartitionRefreshStatus")) && result.ulVal == 3 && FAILED(call(L"getPartitionList")), "scan failure invented candidates");
  services.fail = false; services.delay.store(true);
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "delayed scan start");
  const bool excluded = select(3) == E_PENDING && call(L"openDiskManagement") == E_PENDING && call(L"getVolumeOperationStatus") == E_PENDING;
  work.card_epoch.fetch_add(1); work.interrupted.store(true);
  const bool pending = SUCCEEDED(call(L"getPartitionRefreshStatus")) && result.ulVal == 1;
  services.delay.store(false); wait();
  require(excluded && pending && call(L"getPartitionList") == E_ACCESSDENIED, "worker/session boundary failed");
  work.interrupted.store(false);
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "session rescan"); wait();
  require(SUCCEEDED(call(L"clearPartitions")) && FAILED(call(L"getPartitionList")), "clear retained snapshot");
  for (int scenario = 0; scenario < 6; ++scenario) {
    require(SUCCEEDED(call(L"beginPartitionRefresh")), "fresh validation scan"); wait();
    require(SUCCEEDED(call(L"getPartitionList")), "fresh validation list");
    const auto id = static_cast<unsigned int>(std::stoul(result.bstrVal));
    require(SUCCEEDED(select(id)), "unchanged target rejected");
    services.device_replaced = scenario == 5;
    services.changed = scenario == 0;
    services.missing = scenario == 1;
    services.duplicate = scenario == 2;
    services.fail = scenario == 3;
    services.expire_on_read = scenario == 4 ? &work : nullptr;
    require(FAILED(select(id)), "stale or uncertain target accepted");
    require(FAILED(call(L"getSelectedPartition")), "failed validation retained selection");
    services.changed = services.missing = services.duplicate = services.fail = false;
    services.expire_on_read = nullptr;
    services.device_replaced = false;
  }
  for (bool expire : {false, true}) {
    require(SUCCEEDED(call(L"beginPartitionRefresh")), "helper scan"); wait();
    require(SUCCEEDED(call(L"getPartitionList")), "helper list");
    const auto id = static_cast<unsigned int>(std::stoul(result.bstrVal));
    require(SUCCEEDED(select(id)), "helper select");
    services.check_delay.store(true); services.check_fails = !expire;
    require(SUCCEEDED(call(L"beginPartitionCheck")), "delayed check start");
    const bool check_pending = SUCCEEDED(call(L"getPartitionCheckStatus")) && result.ulVal == 1;
    const bool blocked = call(L"beginPartitionRefresh") == E_PENDING && select(id) == E_PENDING && call(L"openDiskManagement") == E_PENDING;
    if (expire) work.card_epoch.fetch_add(1);
    services.check_delay.store(false); wait();
    require(check_pending && blocked && FAILED(call(L"getSelectedPartition")), "helper failure retained selection or allowed concurrent action");
    if (!expire) require(SUCCEEDED(call(L"getPartitionCheckStatus")) && result.ulVal == 3, "helper failure state missing");
  }
  ::VariantClear(&result);
}

// The removable flow lists external media only, and never starts in-place
// encryption on a filesystem other than NTFS.
void test_external_partition_scope() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  FakePartitionServices services;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, nullptr, &services)), "external host");
  fixture.authentication.establish(root_secret());
  VARIANT result{};
  const auto call = [&](const wchar_t* name) { ::VariantClear(&result); return invoke_no_arguments(*host.Get(), name, &result); };
  const auto number = [&](const wchar_t* name, unsigned id) { ::VariantClear(&result); return invoke_with_number(*host.Get(), name, id, &result, nullptr); };
  const auto wait = [&] {
    for (int i = 0; work.running.load() && i < 3000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(), "external scan timeout");
  };
  const auto list = [&] { require(SUCCEEDED(call(L"getPartitionList")) && result.vt == VT_BSTR, "external list"); return std::wstring(result.bstrVal); };
  require(number(L"beginPartitionRefresh", 2) == DISP_E_TYPEMISMATCH, "unknown scope accepted");
  require(SUCCEEDED(number(L"beginPartitionRefresh", 1)), "external scan"); wait();
  require(list().empty(), "internal volume listed as external");
  services.external_item = true;
  require(SUCCEEDED(number(L"beginPartitionRefresh", 1)), "external rescan"); wait();
  auto rows = list();
  require(rows.find(L'\n') == std::wstring::npos && rows.ends_with(L",1073741824,0"), "external scan kept internal volumes");
  auto id = static_cast<unsigned int>(std::stoul(rows));
  require(SUCCEEDED(number(L"getPartitionNtfs", id)) && result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE, "NTFS fact missing");
  require(number(L"getPartitionNtfs", 999) == E_ACCESSDENIED, "unknown NTFS id accepted");
  require(SUCCEEDED(call(L"beginPartitionRefresh")), "full scan"); wait();
  require(list().find(L'\n') != std::wstring::npos, "full scan dropped volumes");

  services.ntfs_item = false;
  require(SUCCEEDED(number(L"beginPartitionRefresh", 1)), "exFAT scan"); wait();
  id = static_cast<unsigned int>(std::stoul(list()));
  require(SUCCEEDED(number(L"getPartitionNtfs", id)) && result.boolVal == VARIANT_FALSE, "non-NTFS reported as NTFS");
  require(SUCCEEDED(number(L"selectPartition", id)), "exFAT select");
  require(number(L"beginPartitionConversion", id) == E_ACCESSDENIED && fixture.volume.partition_creations == 0,
          "in-place encryption started on a non-NTFS medium");
  require(SUCCEEDED(number(L"beginPartitionRefresh", 1)), "exFAT rescan"); wait();
  id = static_cast<unsigned int>(std::stoul(list()));
  require(SUCCEEDED(number(L"selectPartition", id)) && SUCCEEDED(number(L"beginPartitionCreate", id)), "erasing a non-NTFS medium refused");
  wait();
  require(fixture.volume.partition_creations == 1, "erase on an external medium did not run");
  ::VariantClear(&result);
}

void test_partition_creation_snapshot_and_epoch(const wchar_t* method = L"beginPartitionCreate") {
  for (int scenario = 0; scenario != 3; ++scenario) {
    Fixture fixture;
    usb_vault::ui::CreationWorkState work;
    FakePartitionServices services;
    ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, nullptr, &services)), "partition create host");
    VARIANT result{};
    const auto call = [&](const wchar_t* name) { ::VariantClear(&result); return invoke_no_arguments(*host.Get(), name, &result); };
    const auto number = [&](const wchar_t* name, unsigned id) { ::VariantClear(&result); return invoke_with_number(*host.Get(), name, id, &result, nullptr); };
    const auto wait = [&] {
      for (int i = 0; work.running.load() && i < 3000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(!work.running.load(), "partition create worker timeout");
    };
    require(FAILED(number(method, 1)), "unauthenticated create accepted");
    fixture.authentication.establish(root_secret());
    require(SUCCEEDED(call(L"beginPartitionRefresh")), "create scan"); wait();
    require(SUCCEEDED(number(L"selectPartition", 1)), "create select");
    require(FAILED(number(method, 999)), "unselected create accepted");
    // The invalid request clears selection, so acquire a new scoped identifier.
    require(SUCCEEDED(call(L"beginPartitionRefresh")), "create rescan"); wait();
    require(SUCCEEDED(number(L"selectPartition", 3)), "create reselect");
    if (scenario == 1) work.card_epoch.fetch_add(1);
    if (scenario == 2) work.interrupted.store(true);
    const auto started = number(method, 3);
    if (scenario) {
      require(FAILED(started) && fixture.volume.partition_creations == 0, "expired card created partition");
    } else {
      require(SUCCEEDED(started), "partition create start"); wait();
      require(SUCCEEDED(call(L"getCreationProgress")) && fixture.volume.partition_creations == 1,
          "partition create not routed through creation worker");
      require(FAILED(number(method, 3)), "partition create replay accepted");
    }
    ::VariantClear(&result);
  }
}

void test_partition_pair_bridge() {
  for (int scenario=0; scenario<4; ++scenario) {
    Fixture fixture; fixture.authentication.establish(root_secret());
    usb_vault::ui::CreationWorkState work;
    FakePartitionServices services;
    ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller,nullptr,&host,nullptr,&work,nullptr,&services)),"pair host");
    VARIANT result{};
    require(SUCCEEDED(invoke_no_arguments(*host.Get(),L"beginPartitionRefresh",&result)),"pair scan");
    for(int i=0;work.running.load()&&i<3000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(),"pair scan timeout");
    require(SUCCEEDED(invoke_with_number(*host.Get(),L"selectPartition",1,&result,nullptr)),"pair select");
    ByteArray secret(bytes("second-password-value")), confirmation(bytes("second-password-value"));
    VARIANT args[4]{confirmation.value(),secret.value(),{}, {}};
    args[2].vt=VT_UI4;args[2].ulVal=16;
    args[3].vt=VT_UI4;args[3].ulVal=scenario==1 ? 999 : 1;
    if(scenario==2)work.card_epoch.fetch_add(1);
    DISPPARAMS parameters{args,nullptr,scenario==3 ? 3u : 4u,0};
    const auto status=host->Invoke(member_id(*host.Get(),L"beginPartitionPair"),IID_NULL,LOCALE_INVARIANT,DISPATCH_METHOD,&parameters,&result,nullptr,nullptr);
    secret.require_wiped("pair secret retained");confirmation.require_wiped("pair confirmation retained");
    require(SUCCEEDED(status)==(scenario==0),"pair bridge accepted stale target or invalid arity");
    for(int i=0;work.running.load()&&i<3000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load() && fixture.volume.partition_creations==(scenario==0 ? 1 : 0),"pair worker route failed");
    ::VariantClear(&result);
  }
}

void test_bitlocker_creation_gate() {
  for (const auto method : {L"beginPartitionCreate", L"beginPartitionConversion"}) {
    for (int scenario = 0; scenario < 5; ++scenario) {
      Fixture fixture; fixture.authentication.establish(root_secret());
      usb_vault::ui::CreationWorkState work; FakePartitionServices services;
      services.bitlocker_state.store(1);
      ComPtr<IDispatch> host;
      require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, nullptr, &host, nullptr, &work, nullptr, &services)), "BitLocker host");
      VARIANT result{};
      const auto call = [&](const wchar_t* name) { ::VariantClear(&result); return invoke_no_arguments(*host.Get(), name, &result); };
      const auto wait = [&](auto predicate) {
        for (int i = 0; !predicate() && i < 3000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(predicate(), "BitLocker wait timeout");
      };
      require(FAILED(call(L"confirmPartitionBitLocker")), "idle consent accepted");
      require(SUCCEEDED(call(L"beginPartitionRefresh")), "BitLocker scan"); wait([&] { return !work.running.load(); });
      require(SUCCEEDED(invoke_with_number(*host.Get(), L"selectPartition", 1, &result, nullptr)), "BitLocker select");
      fixture.volume.remember_paths = false;
      require(FAILED(invoke_with_number(*host.Get(), method, 1, &result, nullptr)) && services.decrypt_calls.load() == 0, "decrypted without persistent records");
      fixture.volume.remember_paths = true;
      require(SUCCEEDED(invoke_with_number(*host.Get(), method, 1, &result, nullptr)), "BitLocker begin");
      wait([&] { return SUCCEEDED(call(L"getPartitionBitLockerProgress")) && std::wstring_view(result.bstrVal) == L"1,1,100"; });
      require(services.decrypt_calls.load() == 0 && !fixture.volume.creation_entered.load(), "work ran before consent");
      require(call(L"beginPartitionRefresh") == E_PENDING, "busy refresh accepted");
      if (scenario == 1) require(SUCCEEDED(call(L"cancelContainerCreation")), "cancel consent");
      else if (scenario == 2) { work.card_epoch.fetch_add(1); require(FAILED(call(L"confirmPartitionBitLocker")), "expired consent accepted"); }
      else {
        require(SUCCEEDED(call(L"confirmPartitionBitLocker")) && result.boolVal == VARIANT_TRUE, "confirm failed");
        wait([&] { return services.decrypt_calls.load() == 1; });
        require(!fixture.volume.creation_entered.load(), "creation ran during decryption");
        require(SUCCEEDED(call(L"confirmPartitionBitLocker")) && result.boolVal == VARIANT_FALSE, "consent replay accepted");
        if (scenario == 3) require(SUCCEEDED(call(L"cancelContainerCreation")), "cancel waiting");
        else if (scenario == 4) work.card_epoch.fetch_add(1);
        else services.bitlocker_state.store(0);
      }
      wait([&] { return !work.running.load(); });
      require(fixture.volume.partition_creations == (scenario == 0 ? 1 : 0), "wrong BitLocker continuation");
      require(services.decrypt_calls.load() == (scenario == 1 || scenario == 2 ? 0u : 1u), "unexpected decryption");
      if (scenario == 3 || scenario == 4) require(services.bitlocker_state.load() == 3, "cancel pretended to stop Windows decryption");
      ::VariantClear(&result);
    }
  }
}

void test_partition_resume_pause_and_revocation() {
  for(int scenario=0;scenario<3;++scenario) {
    Fixture fixture;fixture.authentication.establish(root_secret());
    usb_vault::ui::CreationWorkState work;ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller,nullptr,&host,nullptr,&work)),"resume host");
    VARIANT result{};fixture.volume.pause_creation.store(true);
    require(SUCCEEDED(invoke_with_number(*host.Get(),L"beginPartitionResume",42,&result,nullptr)),"resume start failed");
    for(int i=0;!fixture.volume.creation_entered.load() && i<1000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(work.inplace_operation.load(),"window close lacks in-place mode");
    if(scenario!=2){::VariantClear(&result);require(SUCCEEDED(invoke_no_arguments(*host.Get(),L"cancelContainerCreation",&result)),"pause request failed");}
    if(scenario==1)work.card_epoch.fetch_add(1);
    fixture.volume.pause_creation.store(false);
    for(int i=0;work.running.load() && i<3000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!work.running.load(),"resume worker stuck");::VariantClear(&result);
    require(SUCCEEDED(invoke_no_arguments(*host.Get(),L"getCreationProgress",&result)),"resume progress missing");
    require(result.vt==VT_BSTR && std::wstring_view(result.bstrVal).starts_with(scenario==0 ? L"6," : scenario==1 ? L"4," : L"2,"),"normal pause and revocation confused");
    ::VariantClear(&result);
  }
}

void test_leave_application_routes() {
  Fixture fixture;
  RecordingCloseSink sink;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, &sink, &host, nullptr, &work)), "leave host failed");
  const auto invoke = [&](const wchar_t* name) {
    DISPPARAMS args{};
    return host->Invoke(member_id(*host.Get(), name), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &args, nullptr, nullptr, nullptr);
  };
  require(invoke_with_number(*host.Get(), L"returnToLogin", 1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "login return accepted arguments");
  require(invoke_with_number(*host.Get(), L"exitApplication", 1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "exit accepted arguments");
  fixture.authentication.clear_authentication();
  require(invoke(L"returnToLogin") == E_ACCESSDENIED, "unauthenticated login return accepted");
  VARIANTARG tray_arg{}; tray_arg.vt = VT_BOOL; tray_arg.boolVal = VARIANT_TRUE;
  DISPPARAMS tray_args{&tray_arg, nullptr, 1, 0};
  const auto set_tray = [&] { return host->Invoke(member_id(*host.Get(), L"setCloseToTrayEnabled"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &tray_args, nullptr, nullptr, nullptr); };
  require(set_tray() == E_ACCESSDENIED && !sink.tray_enabled, "unauthenticated tray change accepted");
  fixture.authentication.establish(root_secret());
  require(set_tray() == S_OK && sink.tray_enabled, "tray opt-in failed");
  tray_arg.vt = VT_I4; require(set_tray() == DISP_E_TYPEMISMATCH, "nonboolean tray change accepted");
  tray_arg.vt = VT_BOOL;
  sink.allow_tray_write = false; require(set_tray() == E_FAIL, "tray save failure hidden");
  sink.allow_tray_write = true;
  fixture.volume.mounted_state = true;
  require(invoke(L"returnToLogin") == E_ACCESSDENIED, "mounted login return accepted");
  // Exit asks the window guard, even with a mount; the bridge never destroys it.
  require(SUCCEEDED(invoke(L"exitApplication")) && sink.close_calls == 1,
          "exit did not reach normal window guard");
  require(fixture.volume.mounted_state && fixture.authentication.authenticated(),
          "exit bridge discarded active session");
  work.running.store(true);
  require(invoke(L"returnToLogin") == E_PENDING, "return interrupted worker");
  require(set_tray() == E_PENDING, "tray change interrupted worker");
  require(invoke(L"exitApplication") == E_PENDING, "exit interrupted worker");
  work.running.store(false);
  require(sink.login_calls == 0 && sink.close_calls == 1, "denied leave reached sink");
  fixture.volume.mounted_state = false;
  sink.allow_login = false;
  require(FAILED(invoke(L"returnToLogin")), "failed return reported success");
  sink.allow_login = true;
  require(SUCCEEDED(invoke(L"returnToLogin")), "safe login return failed");
  require(invoke(L"getSessionState") == E_PENDING && invoke(L"exitApplication") == E_PENDING,
          "retired bridge still accepts calls");
}

void test_backup_transition_without_login_or_unmount_gate() {
  for (const bool authenticated : {false,true}) {
    Fixture fixture; fixture.entry.route=CardEntryRoute::login_required;
    if (authenticated) fixture.authentication.establish(root_secret());
    fixture.volume.mounted_state=authenticated;
    RecordingCloseSink sink; usb_vault::ui::CreationWorkState work; ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller,&sink,&host,nullptr,&work)),"backup host failed");
    auto invoke=[&]{DISPPARAMS args{};return host->Invoke(member_id(*host.Get(),L"openCardBackup"),IID_NULL,LOCALE_INVARIANT,DISPATCH_METHOD,&args,nullptr,nullptr,nullptr);};
    require(invoke_with_number(*host.Get(),L"openCardBackup",1,nullptr,nullptr)==DISP_E_BADPARAMCOUNT,"backup accepted arguments");
    work.running.store(true); require(invoke()==E_PENDING && sink.backup_calls==0,"backup interrupted work"); work.running.store(false);
    sink.allow_backup=false; require(FAILED(invoke()),"backup rejection reported success");
    sink.allow_backup=true; require(SUCCEEDED(invoke()),"backup required application login or initial unmount");
    require(fixture.volume.mounted_state==authenticated,"backup entry unmounted volume");
    require(invoke()==E_PENDING,"backup bridge was not retired");
    DISPPARAMS args{}; require(host->Invoke(member_id(*host.Get(),L"getSessionState"),IID_NULL,LOCALE_INVARIANT,DISPATCH_METHOD,&args,nullptr,nullptr,nullptr)==E_PENDING,"retired backup bridge accessed context");
    require(usb_vault::ui::resume_card_backup_webview_host_object(host.Get()),"native cancellation did not resume original bridge");
    require(!usb_vault::ui::resume_card_backup_webview_host_object(host.Get()),"bridge resumed twice");
    VARIANT state{};
    require(SUCCEEDED(host->Invoke(member_id(*host.Get(),L"getSessionState"),IID_NULL,LOCALE_INVARIANT,DISPATCH_METHOD,&args,&state,nullptr,nullptr)),"resumed bridge unavailable");
    VariantClear(&state);
    require(fixture.volume.mounted_state==authenticated,"cancellation changed mounts");
    require(SUCCEEDED(invoke()),"cancelled wizard could not reopen");
    fixture.authentication.clear_authentication();
    fixture.volume.mounted_state = false;
    fixture.entry.route = CardEntryRoute::login_required;
    work.card_epoch.fetch_add(1);
    require(usb_vault::ui::resume_card_backup_webview_host_object(host.Get()),"live controller could not resume after revocation");
    require(invoke_state(*host.Get())==2L && !fixture.authentication.authenticated() &&
                !fixture.volume.mounted_state,"resumed bridge restored old authentication or mounts");
  }
}
void test_configuration_transition() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, &g_close_sink, &host, nullptr, &work)), "setup host failed");
  const auto invoke = [&] {
    DISPPARAMS args{};
    return host->Invoke(member_id(*host.Get(), L"openConfiguration"), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &args, nullptr, nullptr, nullptr);
  };
  const int before = g_close_sink.configuration_calls;
  require(invoke_with_number(*host.Get(), L"openConfiguration", 1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "configuration accepted arguments");
  fixture.authentication.establish(root_secret());
  require(invoke() == E_ACCESSDENIED, "authenticated configuration was accepted");
  fixture.authentication.clear_authentication();
  fixture.volume.mounted_state = true;
  require(invoke() == E_ACCESSDENIED, "mounted configuration was accepted");
  fixture.volume.mounted_state = false;

  work.running.store(true);
  require(invoke() == E_PENDING, "configuration interrupted background work");
  work.running.store(false);
  require(g_close_sink.configuration_calls == before, "denied request reached window");
  g_close_sink.allow_configuration = false;
  require(FAILED(invoke()), "failed window request reported success");
  g_close_sink.allow_configuration = true;
  require(SUCCEEDED(invoke()), "unauthenticated configuration failed");
  require(invoke() == E_PENDING, "pending transition accepted another request");
  require(page_event_tests::send(*host.Get()) == S_OK, "configuration transition blocked page reporting");
  DISPPARAMS args{};
  require(host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &args, nullptr, nullptr, nullptr) == E_PENDING,
      "retiring bridge still accessed old context");
}
void test_login_reset_bridge_lifecycle() {
  for (int scenario = 0; scenario < 5; ++scenario) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    RecordingCloseSink sink;
    usb_vault::ui::CreationWorkState work;
    ComPtr<IDispatch> host;
    require(SUCCEEDED(create_card_product_webview_host_object(fixture.controller, &sink, &host, nullptr, &work)), "host creation");
    VARIANT result{};
    VARIANTARG args[2]{}; args[0].vt = VT_BOOL; args[0].boolVal = VARIANT_FALSE;
    args[1].vt = VT_UI4; args[1].ulVal = 1;
    DISPPARAMS parameters{args, nullptr, 2, 0};
    const auto confirm = [&] { return host->Invoke(member_id(*host.Get(), L"beginConfirmLoginReset"), IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &result, nullptr, nullptr); };
    require(FAILED(confirm()) && fixture.entry.login_reset_calls == 0, "direct confirm accepted");
    work.running.store(true);
    require(invoke_no_arguments(*host.Get(), L"beginLoginResetPreparation", &result) == E_PENDING, "busy preparation accepted");
    work.running.store(false);
    fixture.entry.delay_reset_prepare.store(scenario == 4);
    require(SUCCEEDED(invoke_no_arguments(*host.Get(), L"beginLoginResetPreparation", &result)), "prepare failed");
    const auto wait = [&] {
      for (int i = 0; i < 1000; ++i) {
        require(SUCCEEDED(invoke_no_arguments(*host.Get(), L"getLoginResetStatus", &result)), "status failed");
        if (result.ulVal != 1) return result.ulVal;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      throw std::runtime_error("reset worker timeout");
    };
    if (scenario == 4) {
      for (int i = 0; !fixture.entry.reset_prepare_entered.load() && i < 1000; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      work.card_epoch.fetch_add(1);
      fixture.entry.delay_reset_prepare.store(false);
      require(wait() == 6 && fixture.entry.reset_ticket == 0 && sink.close_calls == 0,
              "late prepare restored a retired card confirmation");
      continue;
    }
    require(wait() == 2, "preparation did not finish");
    require(SUCCEEDED(invoke_no_arguments(*host.Get(), L"getLoginResetTicket", &result)), "ticket missing");
    args[1].ulVal = result.ulVal;
    if (scenario == 1) work.card_epoch.fetch_add(1);
    if (scenario == 2) args[1].ulVal++;
    if (scenario == 3) args[0].vt = VT_I4;
    const auto status = confirm();
    if (scenario != 0) {
      require(FAILED(status) && fixture.entry.login_reset_calls == 0 && sink.close_calls == 0, "invalid reset reached card");
    } else {
      require(SUCCEEDED(status) && wait() == 5, "reset failed");
      require(fixture.entry.login_reset_calls == 1 && sink.close_calls == 1, "success did not close once");
      require(FAILED(confirm()) && sink.close_calls == 1, "replay accepted");
    }
  }
}


// P5.14 contact list over the bridge: a card region behind the list, and a
// recheck whose answers the test sets.
struct BridgeContactCard final {
  std::vector<std::byte> bytes =
      std::vector<std::byte>(usb_vault::contacts::kContactStoreV1RegionEnd);
  bool too_small{};
  int leases{};
};

class BridgeContactStorage final : public usb_vault::contacts::IContactStoreStorage {
 public:
  explicit BridgeContactStorage(BridgeContactCard& card) : card_(card) {}
  std::size_t capacity() override { return card_.bytes.size(); }
  SecureBuffer read(const std::size_t offset, const std::size_t length) override {
    if (card_.too_small) throw usb_vault::contacts::ContactStoreCapacityError();
    SecureBuffer out(length);
    std::copy_n(card_.bytes.begin() + static_cast<std::ptrdiff_t>(offset), length, out.data());
    return out;
  }
  void write(const std::size_t offset, const std::span<const std::byte> data) override {
    if (card_.too_small) throw usb_vault::contacts::ContactStoreCapacityError();
    std::ranges::copy(data, card_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  }

 private:
  BridgeContactCard& card_;
};

class BridgeContactLease final : public usb_vault::application::ICardOperationLease {};

class BridgeIdentityVerifier final : public usb_vault::application::ICardIdentityVerifier {
 public:
  using Recheck = usb_vault::application::CardIdentityRecheck;
  [[nodiscard]] Recheck recheck(std::uint32_t) noexcept override { return Recheck::busy; }
  [[nodiscard]] Recheck recheck_before_operation(std::uint32_t) noexcept override {
    return next.value_or(Recheck::same_card);
  }
  std::optional<Recheck> next;
};

[[nodiscard]] SecureBuffer contact_root(const unsigned char fill) {
  SecureBuffer secret(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < secret.size(); ++index) {
    secret.bytes()[index] = static_cast<std::byte>(fill + index);
  }
  return secret;
}

[[nodiscard]] std::wstring hex_key(const usb_vault::contacts::ContactPublicKey& key) {
  std::wstring text;
  constexpr wchar_t kDigits[] = L"0123456789abcdef";
  for (const std::byte value : key) {
    text.push_back(kDigits[std::to_integer<unsigned int>(value) >> 4]);
    text.push_back(kDigits[std::to_integer<unsigned int>(value) & 0x0F]);
  }
  return text;
}

[[nodiscard]] std::wstring widen_ascii(const std::string_view text) {
  return {text.begin(), text.end()};
}

void test_the_contact_list_crosses_the_bridge_as_packed_strings() {
  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume(authentication);
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  BridgeIdentityVerifier verifier;
  BridgeContactCard card;
  ScopedDirectory directory(L"bridge-contacts");
  CardProductUiSessionController controller(entry, authentication, volume, policy, chooser,
                                            nullptr, nullptr, &verifier);
  controller.set_contact_book_access(
      {.storage = [&card](std::uint32_t) -> std::unique_ptr<usb_vault::contacts::IContactStoreStorage> {
         return std::make_unique<BridgeContactStorage>(card);
       },
       .lease = []() -> std::unique_ptr<usb_vault::application::ICardOperationLease> {
         return std::make_unique<BridgeContactLease>();
       }});
  ComPtr<IDispatch> host = make_host(controller);
  const auto call = [&](const wchar_t* const name, std::vector<std::wstring> texts) {
    std::vector<VARIANTARG> values;
    // COM order: the last argument first.
    for (auto it = texts.rbegin(); it != texts.rend(); ++it) values.push_back(text_argument(*it));
    VARIANT result{};
    const HRESULT hr = invoke_named(*host.Get(), name, values.empty() ? nullptr : values.data(),
                                    static_cast<UINT>(values.size()), &result, nullptr);
    for (VARIANTARG& value : values) static_cast<void>(::VariantClear(&value));
    require(hr == S_OK && result.vt == VT_BSTR, "a contact call did not answer a string");
    std::wstring text(result.bstrVal, ::SysStringLen(result.bstrVal));
    static_cast<void>(::VariantClear(&result));
    return text;
  };
  const std::wstring rs(1, L'\x1e');
  const std::wstring fs(1, L'\x1f');

  require(call(L"getContactBook", {}) == L"1", "the list crossed before login");

  entry.route = CardEntryRoute::login_required;
  entry.identity = 0x5EC0;
  auto password = bytes("password-1");
  controller.submit_login(password);
  const std::wstring own = widen_ascii(usb_vault::contacts::format_contact_fingerprint(
      usb_vault::contacts::ContactIdentity::derive(root_secret()).public_key()));
  require(call(L"getContactBook", {}) == L"0" + rs + L"2" + fs + own,
          "an empty list did not cross with its state and own fingerprint");

  const auto alice = usb_vault::contacts::ContactIdentity::derive(contact_root(40)).public_key();
  const auto key_file = std::filesystem::path(directory.wide()) / L"alice.uvkey";
  {
    const std::string text = usb_vault::contacts::encode_contact_key_file(alice);
    std::ofstream(key_file, std::ios::binary) << text;
  }
  require(call(L"chooseContactKeyFile", {}) == L"0" + rs + L"0" + fs, "a cancelled pick crossed as a key");
  chooser.key_file_path = key_file.wstring();
  const std::wstring alice_fp = widen_ascii(usb_vault::contacts::format_contact_fingerprint(alice));
  require(call(L"chooseContactKeyFile", {}) == L"0" + rs + L"1" + fs + alice_fp,
          "a picked key did not cross as its fingerprint");
  // A lone surrogate is not text a remark can hold.
  require(call(L"addChosenContact", {std::wstring(1, static_cast<wchar_t>(0xD800))}) ==
              L"0" + rs + L"10" + fs,
          "a remark that is not text was not refused as invalid");
  require(call(L"addChosenContact", {L"  爱丽丝 "}) == L"0" + rs + L"0" + fs, "a contact was not added");
  require(call(L"addChosenContact", {L"again"}) == L"1", "an added key file was held");
  require(call(L"getContactBook", {}) ==
              L"0" + rs + L"3" + fs + own + rs + hex_key(alice) + fs + L"爱丽丝" + fs + alice_fp,
          "the listed contact did not cross by key, remark and fingerprint");
  static_cast<void>(call(L"chooseContactKeyFile", {}));
  require(call(L"addChosenContact", {L"other"}) == L"0" + rs + L"6" + fs + L"爱丽丝",
          "a listed key did not cross as already present with its remark");

  require(call(L"renameContact", {hex_key(alice), L"Alice"}) == L"0" + rs + L"0", "renaming did not cross");
  std::wstring upper = hex_key(alice);
  for (wchar_t& c : upper) {
    if (c >= L'a' && c <= L'f') c = static_cast<wchar_t>(c - L'a' + L'A');
  }
  require(call(L"renameContact", {upper, L"X"}) == L"8" && call(L"removeContact", {L"zz"}) == L"8",
          "a key not in the form the page was given was taken");
  verifier.next = usb_vault::application::CardIdentityRecheck::different_card;
  require(call(L"removeContact", {hex_key(alice)}) == L"4", "a removed card did not cross as removed");
  verifier.next.reset();
  require(call(L"removeContact", {hex_key(alice)}) == L"0" + rs + L"0" &&
              call(L"removeContact", {hex_key(alice)}) == L"0" + rs + L"11",
          "removal or a missing contact did not cross");
  require(call(L"clearContactBook", {}) == L"1", "an intact list was cleared");

  require(call(L"exportOwnContactKey", {}) == L"0" + rs + L"0", "a cancelled save crossed as saved");
  chooser.key_save_path = (std::filesystem::path(directory.wide()) / L"mine.uvkey").wstring();
  require(call(L"exportOwnContactKey", {}) == L"0" + rs + L"1" &&
              std::filesystem::exists(chooser.key_save_path),
          "the own key file was not saved over the bridge");

  card.too_small = true;
  static_cast<void>(call(L"chooseContactKeyFile", {}));
  require(call(L"addChosenContact", {L"Alice"}) == L"2", "a small card did not cross as too small");
  require(call(L"reloadContactBook", {}) == L"0" + rs + L"1" + fs + own,
          "a small card's list did not cross as unavailable");

  VARIANT result{};
  require(invoke_named(*host.Get(), L"getContactBook", nullptr, 0, nullptr, nullptr) == E_POINTER,
          "a contact call without a result slot was answered");
  VARIANTARG number{};
  number.vt = VT_I4;
  require(invoke_named(*host.Get(), L"removeContact", &number, 1, &result, nullptr) == DISP_E_TYPEMISMATCH &&
              invoke_named(*host.Get(), L"removeContact", nullptr, 0, &result, nullptr) == DISP_E_BADPARAMCOUNT,
          "a contact call of the wrong shape was answered");
}


// A real transfer package of one small file, from `sender` to `recipients`.
[[nodiscard]] std::filesystem::path bridge_transfer_package(
    const ScopedDirectory& directory, const std::wstring& leaf,
    const usb_vault::contacts::ContactIdentity& sender,
    const std::vector<usb_vault::contacts::ContactPublicKey>& recipients) {
  const std::filesystem::path folder = std::filesystem::path(directory.wide()) / leaf;
  std::filesystem::create_directories(folder);
  const std::filesystem::path source = folder / L"note.txt";
  { std::ofstream(source, std::ios::binary) << "transferred content"; }
  const std::array<usb_vault::backup::ExportSelectionSourceV1, 1> sources{
      {{.absolute_source = source.wstring(), .output_name = L"note.txt"}}};
  usb_vault::backup::export_selection_to_file_v1(
      sources, (folder / L"package").wstring(), L"pack",
      usb_vault::backup::ExportRecipientsV1{.sender = sender, .recipients = recipients});
  return folder / L"package";
}

void test_transfers_cross_the_bridge_on_the_worker() {
  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume(authentication);
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  BridgeIdentityVerifier verifier;
  BridgeContactCard card;
  ScopedDirectory directory(L"bridge-transfers");
  CardProductUiSessionController controller(entry, authentication, volume, policy, chooser,
                                            nullptr, nullptr, &verifier);
  controller.set_contact_book_access(
      {.storage = [&card](std::uint32_t) -> std::unique_ptr<usb_vault::contacts::IContactStoreStorage> {
         return std::make_unique<BridgeContactStorage>(card);
       },
       .lease = []() -> std::unique_ptr<usb_vault::application::ICardOperationLease> {
         return std::make_unique<BridgeContactLease>();
       }});
  ComPtr<IDispatch> host = make_host(controller);
  const std::wstring rs(1, L'\x1e');
  const std::wstring fs(1, L'\x1f');
  const auto text_call = [&](const wchar_t* const name, std::vector<std::wstring> texts) {
    std::vector<VARIANTARG> values;
    for (auto it = texts.rbegin(); it != texts.rend(); ++it) values.push_back(text_argument(*it));
    VARIANT result{};
    const HRESULT hr = invoke_named(*host.Get(), name, values.empty() ? nullptr : values.data(),
                                    static_cast<UINT>(values.size()), &result, nullptr);
    for (VARIANTARG& value : values) static_cast<void>(::VariantClear(&value));
    std::wstring text = hr == S_OK && result.vt == VT_BSTR
                            ? std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal))
                            : L"hr:" + std::to_wstring(static_cast<long>(hr));
    static_cast<void>(::VariantClear(&result));
    return text;
  };
  const auto finished = [&] {
    std::wstring status;
    for (int i = 0; i < 20000; ++i) {
      VARIANT value{};
      require(invoke_named(*host.Get(), L"encryptedTransferStatus", nullptr, 0, &value, nullptr) == S_OK,
              "the transfer status was unavailable");
      status.assign(value.bstrVal, ::SysStringLen(value.bstrVal));
      static_cast<void>(::VariantClear(&value));
      if (!status.starts_with(L"1,")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return status;
  };
  const auto verify = [&](const std::filesystem::path& package) {
    chooser.existing_path = package.wstring();
    VARIANT picked{};
    require(invoke_named(*host.Get(), L"chooseImportFile", nullptr, 0, &picked, nullptr) == S_OK,
            "a transfer package could not be picked");
    static_cast<void>(::VariantClear(&picked));
    VARIANT started{};
    require(invoke_named(*host.Get(), L"beginVerifyTransferImport", nullptr, 0, &started, nullptr) == S_OK,
            "a transfer check did not start");
    static_cast<void>(::VariantClear(&started));
    return finished();
  };

  entry.route = CardEntryRoute::login_required;
  entry.identity = 0x5EC0;
  auto password = bytes("password-1");
  controller.submit_login(password);
  const auto own = usb_vault::contacts::ContactIdentity::derive(root_secret()).public_key();
  const auto alice = usb_vault::contacts::ContactIdentity::derive(contact_root(40));
  const auto bob = usb_vault::contacts::ContactIdentity::derive(contact_root(41));
  const auto alice_file = std::filesystem::path(directory.wide()) / L"alice.uvkey";
  { std::ofstream(alice_file, std::ios::binary) << usb_vault::contacts::encode_contact_key_file(alice.public_key()); }
  chooser.key_file_path = alice_file.wstring();
  static_cast<void>(controller.choose_contact_key_file());
  require(controller.add_chosen_contact("Alice").change == usb_vault::application::ContactBookChange::done,
          "the test contact was not added");

  // Export to a listed contact; a key in any other form never starts one.
  const auto export_to = [&](const std::wstring& recipients) {
    std::array<VARIANTARG, 4> values{};
    values[3] = text_argument(L"3" + fs + fs + L"q1.txt");
    values[2] = text_argument(L"D:\\Out");
    values[1] = text_argument(L"pack");
    values[0] = text_argument(recipients);
    VARIANT started{};
    const HRESULT hr = invoke_named(*host.Get(), L"beginRecipientExport", values.data(), 4, &started, nullptr);
    clear_arguments(values.data(), 4);
    static_cast<void>(::VariantClear(&started));
    return hr;
  };
  require(export_to(L"nothex") == E_INVALIDARG && export_to(hex_key(alice.public_key()) + rs) == E_INVALIDARG,
          "a malformed recipient list started an export");
  require(export_to(hex_key(alice.public_key())) == S_OK && finished().starts_with(L"2,") &&
              volume.recipient_exports.size() == 1 &&
              volume.recipient_exports[0] == std::vector{alice.public_key()},
          "a recipient export did not reach the session for the chosen contact");
  require(export_to(hex_key(bob.public_key())) == S_OK && finished().starts_with(L"3,") &&
              volume.recipient_exports.size() == 1,
          "an unlisted recipient was exported to");

  // A listed sender, then the import into an existing container.
  const auto from_alice = bridge_transfer_package(directory, L"alice", alice, {own});
  require(verify(from_alice).starts_with(L"2,") &&
              text_call(L"transferImportSender", {}) == L"2" + rs + L"0" + fs + L"Alice" + fs,
          "a listed sender did not cross by remark");
  std::array<VARIANTARG, 2> into{};
  into[1] = number_argument(5);
  into[0] = text_argument(L"in");
  VARIANT started{};
  require(invoke_named(*host.Get(), L"beginImportVerified", into.data(), 2, &started, nullptr) == S_OK &&
              finished().starts_with(L"2,"),
          "a verified transfer was not imported");
  clear_arguments(into.data(), 2);
  static_cast<void>(::VariantClear(&started));
  require(volume.transfer_imports == std::vector<std::uint32_t>{5}, "the transfer import did not reach the container");

  // An unlisted sender by fingerprint, and adding it.
  const auto from_bob = bridge_transfer_package(directory, L"bob", bob, {own});
  require(verify(from_bob).starts_with(L"2,") &&
              text_call(L"transferImportSender", {}) ==
                  L"2" + rs + L"0" + fs + fs +
                      widen_ascii(usb_vault::contacts::format_contact_fingerprint(bob.public_key())),
          "an unlisted sender did not cross by fingerprint");
  require(text_call(L"addTransferSender", {L"Bob"}) == L"0" + rs + L"0" + fs, "a verified sender was not added");

  // Not addressed here: the page asks for the password.
  const auto elsewhere = bridge_transfer_package(directory, L"elsewhere", alice, {bob.public_key()});
  require(verify(elsewhere).starts_with(L"2,") && text_call(L"transferImportSender", {}) == L"1",
          "a package for another card did not fall back to the password");

  // Damage fails the check; a forged signature has its own code below it.
  const auto damaged = bridge_transfer_package(directory, L"damaged", bob, {own});
  {
    std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(-20, std::ios::end);
    file.put('\x5A');
  }
  const std::wstring failed = verify(damaged);
  require(failed.starts_with(L"3,") && failed.find(L",10,") != std::wstring::npos &&
              text_call(L"transferImportSender", {}) == L"0",
          ("a damaged transfer package was not refused as damaged: " + usb_vault::diagnostics::utf8(failed)).c_str());

  // Without a usable contact list the check is not made; the password path stays.
  controller.set_contact_book_access({});
  controller.release_stale_contact_book();
  authentication.clear_authentication();
  entry.route = CardEntryRoute::login_required;
  auto again = bytes("password-1");
  controller.submit_login(again);
  require(verify(from_alice).starts_with(L"2,") && text_call(L"transferImportSender", {}) == L"3",
          "a missing contact list closed the password path");
  authentication.clear_authentication();
  require(text_call(L"transferImportSender", {}) == L"0", "a sender outlived its login");
}

}  // namespace

void test_container_alias_bridge_is_authenticated_bounded_and_exclusive() {
  Fixture fixture;
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, &g_close_sink, &host, nullptr, &work)), "rename host creation failed");
  const auto rename = [&](const std::wstring& name, std::uint32_t id=1) {
    std::array<VARIANTARG,2> args{text_argument(name),number_argument(id)};
    const auto result=invoke_named(*host.Get(),L"renameContainer",args.data(),2,nullptr,nullptr);
    clear_arguments(args.data(),2);
    return result;
  };
  require(FAILED(rename(L"资料")) && !fixture.volume.renamed_container_id, "unauthenticated bridge rename accepted");
  fixture.authentication.establish(root_secret());
  require(rename(L"资料 \U0001F512",7)==S_OK && fixture.volume.renamed_container_id==7 &&
          fixture.volume.container_alias==L"资料 \U0001F512", "alias bridge changed identity or text");
  for (const auto& invalid : {std::wstring(L"a/b"),std::wstring(L"a\\b"),std::wstring(L"  "),
                             std::wstring(L"a\0b",3),std::wstring(101,L'x')})
    require(rename(invalid)==E_INVALIDARG, "malformed alias crossed bridge");
  require(rename(L"bad",0)==E_INVALIDARG, "zero ID crossed rename bridge");
  require(invoke_named(*host.Get(),L"renameContainer",nullptr,0,nullptr,nullptr)==DISP_E_BADPARAMCOUNT,
          "wrong arity accepted");
  std::array<VARIANTARG,2> bad{number_argument(1),number_argument(7)};
  require(invoke_named(*host.Get(),L"renameContainer",bad.data(),2,nullptr,nullptr)==DISP_E_TYPEMISMATCH,
          "numeric alias coerced to text");
  work.running.store(true);
  require(rename(L"busy")==E_PENDING, "rename raced background work");
  work.running.store(false);
  work.interrupted.store(true);
  require(rename(L"revoked")==E_ACCESSDENIED, "interrupted worker accepted rename");
  work.interrupted.store(false);
  fixture.volume.fail_container_rename=true;
  require(rename(L"failed")==E_FAIL && fixture.volume.container_alias==L"资料 \U0001F512", "failed rename reported success");
  fixture.volume.fail_container_rename=false;
  require(rename(L"")==S_OK && fixture.volume.container_alias.empty(), "alias reset refused");
}

int main() {
  try {
    test_activation_bridge_gates_and_renews();
    test_container_alias_bridge_is_authenticated_bounded_and_exclusive();
    test_exit_hotkey_settings_bridge();
    test_partition_pair_bridge();
    test_bitlocker_creation_gate();
    test_partition_selection_lifecycle();
    test_explorer_opens_only_a_mounted_session_container();
    test_unmount_batch_refreshes_this_pc_once();
    test_retired_host_never_reaches_its_controller();
    test_partition_resume_pause_and_revocation();
    test_partition_creation_snapshot_and_epoch();
    test_partition_creation_snapshot_and_epoch(L"beginPartitionConversion");
    test_external_partition_scope();
    test_login_reset_bridge_lifecycle();
    test_protection_query_is_boolean_scoped_and_serialized();
    test_import_capacity_warning_rejects_stale_events_and_shares_protection_latch();
    test_leave_application_routes();
    test_backup_transition_without_login_or_unmount_gate();
    test_configuration_transition();
    test_surface_and_state_projection_are_fixed();
    test_window_size_presets_are_bounded();
    test_file_visibility_bridge_guards();
    test_container_extension_preference_is_authenticated_and_bounded();
    test_operation_logging_setting_is_authenticated_and_boolean();
    test_background_creation_reports_progress_and_excludes_concurrent_calls();
    test_a_running_creation_can_be_called_off();
    test_creation_finalization_rejects_late_cancellation();
    test_login_consumes_and_wipes_one_array();
    test_runtime_log_records_page_calls();
    test_page_reports_share_quota();
    test_initialization_preserves_com_order_and_wipes_both_arrays();
    test_initialization_failure_wipes_every_supported_argument();
    test_bad_arity_still_wipes_supplied_password_array();
    test_unmount_and_property_rejection();
    test_container_collection_is_exposed_by_position_and_route_gated();
    test_container_calls_reject_wrong_shapes_and_report_cancellation();
    test_directory_browse_reports_the_choice_as_a_string();
    test_adopted_container_mount_and_unmount_route_through_identifiers();
    test_background_volume_operations();
    test_selected_batches_are_serial_bounded_and_session_scoped();
    test_the_remember_switch_crosses_the_bridge_as_a_boolean();
    test_the_import_switch_crosses_the_bridge_as_a_boolean();
    test_container_b_creation_consumes_six_reversed_arguments();
    test_container_b_creation_consumes_six_reversed_arguments(true);
    test_container_query_failure_does_not_invent_facts();
    test_operation_history_is_read_only_typed_and_clearable();
    test_operation_history_query_failures_do_not_invent_facts();
    test_a_decoy_survey_walks_on_a_worker_and_reports_identifiers();
    test_selection_export_bridge_parses_bounds_and_wipes_inputs();
    test_an_encrypted_export_runs_on_the_worker();
    test_an_encrypted_import_runs_on_the_worker();
    test_the_import_package_rename_bridge_validates_and_reports_collisions();
    test_an_import_into_a_new_container_needs_a_verified_package();
    test_a_decoy_survey_stops_when_asked_and_refuses_what_is_not_a_password();
    test_opening_maintenance_runs_on_the_volume_worker();
    test_maintenance_facts_are_async_current_and_session_scoped();
    test_a_directory_listing_arrives_as_one_packed_string();
    test_a_failed_listing_says_which_way_it_failed();
    test_the_file_manager_bridge_checks_its_shapes();
    test_the_bridge_carries_each_file_manager_action();
    test_the_bridge_carries_both_exports();
    test_the_export_bridge_checks_its_shapes();
    test_the_contact_list_crosses_the_bridge_as_packed_strings();
    test_transfers_cross_the_bridge_on_the_worker();
    test_the_page_names_a_drop_destination_and_reads_only_numbers();
    test_a_running_drop_answers_only_about_itself();
    std::cout << "card_product_webview_host_object_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_webview_host_object_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
