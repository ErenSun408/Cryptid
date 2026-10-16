#include "application/card_login_mount_session_internal.h"
#include "volume/mount_completion.h"

#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/application/container_index_v1.h"
#include "usb_vault/backup/import_package_v1.h"
#include "usb_vault/application/container_index_view_v1.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <cstddef>
#include <algorithm>
#include <concepts>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardLoginMountSession;
using usb_vault::application::ContainerLayout;
using usb_vault::application::ContainerStatus;
using usb_vault::application::detail::SessionContainer;
using usb_vault::application::ContainerCreationCleanupError;
using usb_vault::application::DecoyMaintenanceRefused;
using usb_vault::application::DecoyMaintenanceSpace;
using usb_vault::application::DecoyMaintenanceSurvey;
using usb_vault::application::OperationEvent;
using usb_vault::application::OperationKind;
using usb_vault::application::OperationOutcome;
using usb_vault::application::detail::CardLoginMountSessionCore;
using usb_vault::application::detail::CardLoginMountSessionRuntime;
using usb_vault::application::container_index_v1_mutable_slot;
using usb_vault::application::derive_shared_container_index_key_v1;
using usb_vault::application::fill_container_index_slot_v1;
using usb_vault::application::kContainerIndexV1FileSize;
using usb_vault::application::kContainerIndexV1SlotCount;
using usb_vault::application::list_container_index_paths_v1;
using usb_vault::security::SecureBuffer;
using usb_vault::volume::VolumeDirectoryEntry;
using usb_vault::volume::VolumeDirectoryListing;

static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::ICardLoginSuccessConsumer>);
static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::IActiveVolumeSession>);

// 🔴 The operation history is a closed schema, and Q-04 turns that from a
// design preference into an obligation: an export kind here would mean the
// product records that an export happened, which is precisely what the user
// did not agree to carry. Pinning the four values makes adding a fifth a
// deliberate act that has to come through this line.
static_assert(static_cast<std::uint8_t>(OperationKind::session_opened) == 0);
static_assert(static_cast<std::uint8_t>(OperationKind::container_created) == 1);
static_assert(static_cast<std::uint8_t>(OperationKind::container_mounted) == 2);
static_assert(static_cast<std::uint8_t>(OperationKind::container_unmounted) ==
              3);

// The values above pin what each kind is on the wire; this pins that there are
// only those four. A fifth enumerator makes the switch below incomplete, and
// C4062 is raised to an error right here so that becomes a build failure -- an
// export kind cannot be added to the history quietly, which is the whole point
// of Q-04's third acceptance item.
//
// The pragma is not optional: C4062 is off by default in MSVC and `/W4` does
// not turn it on, so a switch like this one is silent about the very thing it
// is here to catch unless the warning is asked for by number.
#pragma warning(push)
#pragma warning(error : 4062)
[[nodiscard]] constexpr bool operation_kind_is_closed(
    const OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::session_opened:
    case OperationKind::container_created:
    case OperationKind::container_mounted:
    case OperationKind::container_unmounted:
      return true;
  }
  return false;
}
#pragma warning(pop)

static_assert(operation_kind_is_closed(OperationKind::session_opened));

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::vector<std::byte> bytes_vector(
    const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] SecureBuffer bytes(const std::string_view value) {
  SecureBuffer result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(value[index]);
  }
  return result;
}

// Whether a password is this fake's B derivation of one particular input,
// which is how the outer and the hidden side are told apart.
[[nodiscard]] bool is_b_derivation(const std::span<const std::byte> password,
                                   const std::string_view input) {
  std::string material = "derived-b-";
  material.append(input);
  material.resize(28, '.');
  if (password.size() != material.size()) return false;
  for (std::size_t index = 0; index < material.size(); ++index) {
    if (password[index] != static_cast<std::byte>(material[index])) return false;
  }
  return true;
}

[[nodiscard]] SecureBuffer root_secret(
    const std::size_t size =
        usb_vault::card::kCardRecordV1RootSecretSize) {
  SecureBuffer result(size);
  for (std::size_t index = 0; index < size; ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

[[nodiscard]] ContainerLayout layout() {
  ContainerLayout result;
  result.container_a.path = L"C:\\vault\\a.hc";
  result.container_a.drive_letter = L'X';
  result.container_b.path = L"C:\\vault\\b.hc";
  result.container_b.drive_letter = L'Y';
  // The two containers derive under different salts, exactly as the
  // production layout does; otherwise this fixture cannot tell them apart.
  result.container_b.salt.fill(std::byte{0xB0});
  return result;
}

class FakeRuntime final : public CardLoginMountSessionRuntime {
 public:
  bool hiding_enabled{};
  bool actual_hidden{};
  bool container_file_hidden(std::wstring_view) override { return actual_hidden; }
  bool hiding_fails{};
  std::wstring visibility_path;
  bool hide_new_file_containers() override { return hiding_enabled; }
  void set_container_file_hidden(std::wstring_view path, bool hidden) override {
    visibility_path = path;
    events.emplace_back("file-attributes");
    if (hiding_fails) throw std::runtime_error("attribute update failed");
    actual_hidden = hidden;
  }
  std::vector<std::wstring> protected_directories;
  bool protect_directory_fails{};
  bool remove_directory_fails{};
  void create_protected_directory(std::wstring_view path) override {
    events.emplace_back("directory:create");
    if (protect_directory_fails) throw std::runtime_error("directory unavailable");
    protected_directories.emplace_back(path);
  }
  void remove_empty_directory(std::wstring_view path) override {
    events.emplace_back("directory:remove");
    if (remove_directory_fails) throw std::runtime_error("directory busy");
    std::erase(protected_directories, std::wstring(path));
  }
  bool partition_mount_fail{};
  bool partition_pair{};
  std::function<void()> after_partition_mount;
  // A synthetic provider result lets the session test verify that it uses
  // the returned ownership handle, independently of its allocation request.
  wchar_t partition_lands_on{};
  usb_vault::volume::MountResult mount_partition(const usb_vault::volume::PartitionIdentity& identity,
      const SecureBuffer& password, const usb_vault::volume::MountOptions& options) override {
    require(usb_vault::volume::valid_partition_identity(identity) && password.size() == 64,
            "partition mount identity or password invalid");
    require(options.prf == usb_vault::volume::Pkcs5Prf::sha512 && options.pim == 0 &&
                !options.read_only && !options.notify_shell,
            "partition mount received other fixed options than a file mount");
    events.emplace_back("mount:partition");
    last_mount_label = options.label;
    if (partition_mount_fail) throw std::runtime_error("partition unavailable");
    const wchar_t landed =
        partition_lands_on ? partition_lands_on : options.drive_letter;
    if (options.hidden_volume_protection) {
      if (!partition_pair) throw usb_vault::application::DecoyMaintenanceRefused();
      attached_volume_types[landed] = usb_vault::volume::VolumeType::outer;
      identities[landed] = {.drive_letter=landed,
          .unique_id=next_mount_id++, .volume_path=L"partition",
          .disk_length=identity.length_bytes-262144, .protected_outer_writable=true};
    } else if (partition_pair && password.bytes()[0] == std::byte{'p'})
      throw std::system_error(usb_vault::volume::make_error_code(usb_vault::volume::DriverError::authentication_failed));
    usb_vault::volume::detail::LockedDriverBuffer<usb_vault::volume::detail::abi::MountRequest> request;
    request.get().drive_number = static_cast<std::int32_t>(landed - L'A');
    return usb_vault::volume::detail::complete_mount(request, [&](auto&) {
      attached_volume_sizes[landed] = identity.length_bytes - 262144;
      if (after_partition_mount) after_partition_mount();
    });
  }
  bool partition_password_64{};
  bool partition_skip_confirmation{};
  bool partition_fail{};
  bool partition_written{};
  std::function<void()> before_partition_confirmation;
  std::function<void()> after_partition_confirmation;
  std::function<void()> after_partition_write;
  void format_partition(const usb_vault::volume::PartitionCandidate&,
      const SecureBuffer& password, const std::function<bool()>& confirm,
      const usb_vault::application::CreationCancellation& cancelled) override {
    events.emplace_back("format:partition");
    require(password.size() == 64, "partition password must be binary 64 bytes");
    if (before_partition_confirmation) before_partition_confirmation();
    if (partition_skip_confirmation) return;
    if (cancelled() || !confirm()) throw std::runtime_error("partition refused");
    require(!confirm(), "erase consent was reused");
    if (after_partition_confirmation) after_partition_confirmation();
    if (cancelled() || partition_fail) throw std::runtime_error("partition stopped");
    partition_written = true;
    if (after_partition_write) after_partition_write();
  }
  void format_partition_pair(const usb_vault::volume::PartitionCandidate& target, std::uint64_t hidden,
      const SecureBuffer& outer, const SecureBuffer& inner, const std::function<bool()>& confirm,
      const usb_vault::application::CreationCancellation& cancelled) override {
    require(hidden == 16ULL * 1024 * 1024 && outer.size() == 64 && inner.size() == 64 &&
        outer.bytes()[0] == std::byte{'d'} && inner.bytes()[0] == std::byte{'d'}, "pair derivations invalid");
    format_partition(target, outer, confirm, cancelled);
    partition_pair = true;
  }
  bool conversion_pause{true}, conversion_fail{}, wrong_conversion_target{};
  std::function<void()> during_conversion;
  usb_vault::volume::PartitionCandidate conversion_target;
  usb_vault::volume::PartitionCandidate resolve_conversion_target(const usb_vault::volume::PartitionIdentity&) override {
    auto target=conversion_target; if(wrong_conversion_target)target.device_id[0]^=1; return target;
  }
  bool convert_partition(const usb_vault::volume::PartitionCandidate& target, bool,
      const SecureBuffer& password,const std::function<bool()>& confirm,
      const usb_vault::application::CreationCancellation& revoked,
      const usb_vault::application::CreationCancellation&) override {
    require(password.size()==64,"conversion secret invalid");
    if(revoked() || !confirm())throw std::runtime_error("conversion refused");
    require(!confirm(),"conversion reused consent");
    conversion_target=target;
    if(during_conversion)during_conversion();
    if(conversion_fail || revoked())throw std::runtime_error("conversion failed");
    return conversion_pause;
  }
  bool saved_logging{true};
  bool saved_force{};
  bool saved_auto{};
  bool auto_mount_enabled() override { return saved_auto; }
  void set_auto_mount_enabled(bool enabled) override {
    if (fail_preference_write) throw std::runtime_error("settings write failed");
    saved_auto = enabled;
  }
  bool fail_preference_write{};
  bool operation_logging_enabled() override { return saved_logging; }
  void set_operation_logging_enabled(bool enabled) override {
    if (fail_preference_write) throw std::runtime_error("settings write failed");
    saved_logging = enabled;
  }
  bool force_unmount_enabled() override { return saved_force; }
  void set_force_unmount_enabled(bool enabled) override { saved_force = enabled; }
  bool fail_unmount_a{};
  bool fail_unmount_b{};
  bool fail_manual_force{};
  wchar_t fail_manual_force_drive{};
  int forced_unmount_attempts{};
  bool force_required_a{};
  bool force_required_b{};
  bool container_a_exists{};
  bool staging_exists{};
  bool fail_create{};
  bool fail_publish{};
  bool fail_remove{};
  bool fail_created_mount{};
  bool fail_notify{};
  std::uint64_t container_a_size_bytes{64ULL * 1024ULL * 1024ULL};
  std::vector<wchar_t> free_drive_letters{L'X', L'W', L'V'};
  bool fail_allocate{};
  std::uint64_t other_size_bytes{};
  std::uint64_t outer_size{};
  std::uint64_t hidden_size{};
  std::vector<std::byte> hidden_outer_password;
  std::vector<std::byte> hidden_inner_password;
  std::vector<std::byte> last_mount_password;
  std::wstring last_mount_label;
  std::vector<std::wstring> other_existing;
  std::vector<std::wstring> other_staging;
  std::vector<std::string> events;

  int batch_unmounts{};
  int batch_forced_unmounts{};
  [[nodiscard]] usb_vault::volume::UnmountResult unmount_batch_item(
      const wchar_t drive_letter) override {
    ++batch_unmounts;
    return unmount(drive_letter);
  }
  [[nodiscard]] usb_vault::volume::UnmountEscalationResult unmount_forced_batch_item(
      const wchar_t drive_letter) override {
    ++batch_forced_unmounts;
    return unmount_forced(drive_letter);
  }

  [[nodiscard]] usb_vault::volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    attached_volume_sizes.erase(drive_letter);
    attached_volume_types.erase(drive_letter);
    if (drive_letter == L'Y') {
      events.emplace_back("unmount:B");
      if (fail_unmount_b) throw std::runtime_error("unmount B failed");
      return {};
    }
    if (drive_letter == L'X') {
      events.emplace_back("unmount:A");
      if (fail_unmount_a) throw std::runtime_error("unmount A failed");
      return {};
    }
    events.emplace_back("unmount:other");
    return {};
  }

  [[nodiscard]] usb_vault::volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    ++forced_unmount_attempts;
    if (fail_manual_force || drive_letter == fail_manual_force_drive)
      throw std::runtime_error("forced cleanup failed");
    attached_volume_sizes.erase(drive_letter);
    attached_volume_types.erase(drive_letter);
    if (drive_letter == L'Y') {
      events.emplace_back("force:B");
      return {.result = {}, .forced = force_required_b};
    }
    if (drive_letter == L'X') {
      events.emplace_back("force:A");
      return {.result = {}, .forced = force_required_a};
    }
    events.emplace_back("force:other");
    return {.result = {}, .forced = false};
  }

  [[nodiscard]] bool container_exists(
      const std::wstring_view path) override {
    if (path == L"C:\\vault\\a.hc") {
      events.emplace_back("exists:final");
      return container_a_exists;
    }
    if (path == L"C:\\vault\\a.hc.preparing") {
      events.emplace_back("exists:stage");
      return staging_exists;
    }
    const bool staging = path.ends_with(L".preparing");
    events.emplace_back(staging ? "exists:stage-other" : "exists:final-other");
    const std::vector<std::wstring>& known =
        staging ? other_staging : other_existing;
    for (const std::wstring& present : known) {
      if (present == path) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<std::uint64_t> container_size_if_present(
      const std::wstring_view path) override {
    if (path == L"C:\\vault\\a.hc") {
      events.emplace_back("size:A");
      return container_a_exists
                 ? std::optional<std::uint64_t>{container_a_size_bytes}
                 : std::nullopt;
    }
    events.emplace_back("size:other");
    for (const std::wstring& present : other_existing) {
      if (present == path) {
        return std::optional<std::uint64_t>{other_size_bytes};
      }
    }
    return std::nullopt;
  }

  // The driver's own view: what capacity is attached to a drive letter right
  // now. It is deliberately not the file size - for a container with a hidden
  // volume the two differ, which is the whole reason the session asks here.
  wchar_t fail_volume_size_drive{};
  [[nodiscard]] std::optional<std::uint64_t> mounted_volume_size(
      const wchar_t drive_letter) override {
    events.emplace_back("volume-size");
    if (drive_letter == fail_volume_size_drive) {
      throw std::runtime_error("volume query failed");
    }
    const auto entry = attached_volume_sizes.find(drive_letter);
    if (entry == attached_volume_sizes.end()) return std::nullopt;
    return entry->second;
  }

  [[nodiscard]] usb_vault::volume::VolumeSpace volume_space(
      const wchar_t drive_letter) override {
    events.emplace_back("space");
    space_drive = drive_letter;
    return {.total_bytes = reported_total, .free_bytes = reported_free};
  }

  std::map<wchar_t, usb_vault::volume::MountIdentity> identities;
  std::int32_t next_mount_id{};
  bool fail_identity{};
  bool change_identity_during_measure{};
  [[nodiscard]] usb_vault::volume::MountIdentity mount_identity(wchar_t drive) override {
    if (fail_identity || !attached_volume_sizes.contains(drive))
      throw std::runtime_error("identity unavailable");
    auto result=identities.at(drive);
    result.protected_outer_writable = attached_volume_types.contains(drive) &&
        attached_volume_types.at(drive)==usb_vault::volume::VolumeType::outer;
    return result;
  }
  [[nodiscard]] std::uint64_t measure_protected_space(
      std::wstring_view path, const usb_vault::volume::MountIdentity& identity,
      const SecureBuffer& outer, const SecureBuffer& hidden) override {
    const auto drive_letter=identity.drive_letter;
    require(path==identity.volume_path && identity==mount_identity(drive_letter),
            "measurement received another mount identity");
    require(is_b_derivation(outer.bytes(), protected_outer_input) &&
            is_b_derivation(hidden.bytes(), "login-password"),
            "measurement did not receive authenticated derived credentials");
    events.emplace_back("measure");
    ++measure_calls;
    maintenance_helper_open = true;
    measured_drive = drive_letter;
    if (fail_measure) throw std::runtime_error("measurement refused");
    if(change_identity_during_measure) ++identities.at(drive_letter).unique_id;
    return reported_safe;
  }

  bool maintenance_helper_open{};
  void end_measurements() noexcept override { ++end_measure_calls; maintenance_helper_open = false; }
  unsigned end_import_calls{};
  unsigned protected_import_calls{};
  bool change_identity_during_import{};
  void end_imports() noexcept override { ++end_import_calls; maintenance_helper_open = false; }
  void import_protected(std::wstring_view container, const usb_vault::volume::MountIdentity& identity,
      const SecureBuffer& outer, const SecureBuffer& hidden, std::wstring_view source,
      std::wstring_view destination, const usb_vault::volume::TransferCancellation& cancelled, const std::function<void(unsigned int)>& progress) override {
    require(container==identity.volume_path && identity==mount_identity(identity.drive_letter),"import received wrong identity");
    require(is_b_derivation(outer.bytes(),protected_outer_input)&&is_b_derivation(hidden.bytes(),"login-password"),"import missing authenticated credentials");
    ++protected_import_calls;
    if(progress)progress(50);
    copy_entry(source,std::wstring(1,identity.drive_letter)+L":\\"+std::wstring(destination),cancelled);
    if(change_identity_during_import) ++identities.at(identity.drive_letter).unique_id;
  }

  [[nodiscard]] std::optional<usb_vault::volume::VolumeType>
  mounted_volume_type(const wchar_t drive_letter) override {
    events.emplace_back("volume-type");
    const auto entry = attached_volume_types.find(drive_letter);
    if (entry == attached_volume_types.end()) return std::nullopt;
    return entry->second;
  }

  [[nodiscard]] SecureBuffer derive_container_password(
      const SecureBuffer& password_input,
      const SecureBuffer& root,
      const usb_vault::security::KdfV1Salt& salt) override {
    require(root.size() == usb_vault::card::kCardRecordV1RootSecretSize,
            "creation received the wrong authenticated root");
    if (salt == layout().container_b.salt) {
      events.emplace_back("derive:B");
      std::string material = "derived-b-";
      for (const std::byte value : password_input.bytes()) {
        material.push_back(static_cast<char>(value));
      }
      material.resize(partition_password_64 ? 64 : 28, '.');
      return bytes(material);
    }
    events.emplace_back("derive:A");
    require(password_input.size() == 5 &&
                password_input.bytes()[0] == std::byte{'d'} &&
                password_input.bytes()[1] == std::byte{'i'} &&
                password_input.bytes()[2] == std::byte{'s'} &&
                password_input.bytes()[3] == std::byte{'k'} &&
                password_input.bytes()[4] == std::byte{'A'},
            "creation did not use the fixed diskA input");
    require(salt == layout().container_a.salt,
            "every standard container must share the fixed A salt");
    if (partition_password_64) return bytes(std::string(64, 'p'));
    return bytes("derived-container-a-password");
  }

  void create_standard(
      const std::wstring_view target_path,
      const std::uint64_t size_bytes,
      const SecureBuffer& volume_password) override {
    require(!maintenance_helper_open, "maintenance helper still owns the elevation slot");
    const bool is_container_a = target_path == L"C:\\vault\\a.hc.preparing";
    events.emplace_back(is_container_a ? "create:A" : "create:other");
    require(target_path.ends_with(L".preparing"),
            "creation did not use the staging path");
    require(size_bytes != 0 && size_bytes % 512ULL == 0,
            "creation received the wrong size");
    // The A derivation is 64 bytes in a session that also makes disk
    // containers, so the size follows the derivation rather than a literal.
    require(volume_password.size() == (partition_password_64 ? 64u : 28u),
            "creation received the wrong derived password");
    if (fail_create) throw std::runtime_error("create failed");
    if (is_container_a) {
      container_a_size_bytes = size_bytes;
      staging_exists = true;
    } else {
      other_size_bytes = size_bytes;
      other_staging.emplace_back(target_path);
    }
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer& outer_password,
      const SecureBuffer& hidden_password) override {
    require(!maintenance_helper_open, "maintenance helper still owns the elevation slot");
    events.emplace_back("create:hidden");
    require(target_path.ends_with(L".preparing"),
            "hidden creation did not use a staging path");
    hidden_target_paths.emplace_back(target_path);
    require(outer_size_bytes != 0 && outer_size_bytes % 512ULL == 0 &&
                hidden_size_bytes % 512ULL == 0 &&
                hidden_size_bytes < outer_size_bytes,
            "hidden creation received unusable sizes");
    require(outer_password.size() == 28 && hidden_password.size() == 28,
            "hidden creation received the wrong derived passwords");
    outer_size = outer_size_bytes;
    hidden_size = hidden_size_bytes;
    std::wstring published(target_path);
    published.resize(published.size() - std::wstring_view(L".preparing").size());
    hidden_volume_sizes[published] = hidden_size_bytes;
    hidden_outer_password.assign(outer_password.bytes().begin(),
                                 outer_password.bytes().end());
    hidden_inner_password.assign(hidden_password.bytes().begin(),
                                 hidden_password.bytes().end());
    require(hidden_outer_password != hidden_inner_password,
            "both volumes were created with the same password");
    if (fail_create) throw std::runtime_error("create failed");
    other_staging.emplace_back(target_path);
  }

  void publish_created(const std::wstring_view staging_path,
                       const std::wstring_view target_path) override {
    const bool is_container_a = target_path == L"C:\\vault\\a.hc";
    events.emplace_back(is_container_a ? "publish:A" : "publish:other");
    require(staging_path == std::wstring(target_path) + L".preparing",
            "creation published the wrong paths");
    if (fail_publish) throw std::runtime_error("publish failed");
    if (is_container_a) {
      staging_exists = false;
      container_a_exists = true;
    } else {
      std::erase(other_staging, std::wstring(staging_path));
      other_existing.emplace_back(target_path);
    }
  }

  void remove_incomplete(const std::wstring_view staging_path) override {
    events.emplace_back(staging_path == L"C:\\vault\\a.hc.preparing" ? "cleanup:A" : "cleanup:other");
    require(staging_path.ends_with(L".preparing"),
            "creation cleaned the wrong path");
    if (fail_remove) throw std::runtime_error("cleanup failed");
    staging_exists = false;
    std::erase(other_staging, std::wstring(staging_path));
  }

  [[nodiscard]] usb_vault::volume::MountResult mount_created(
      const std::wstring_view path,
      const SecureBuffer& volume_password,
      const usb_vault::volume::MountOptions& options) override {
    const bool is_container_a = path == L"C:\\vault\\a.hc";
    events.emplace_back(is_container_a ? "mount-created:A"
                                       : "mount-created:other");
    require(!is_container_a || container_a_exists,
            "creation mounted before publication");
    require(volume_password.size() == (partition_password_64 ? 64u : 28u),
            "created mount received the wrong derived password");
    require(options.drive_letter == allocated_drive_letter &&
                options.prf == usb_vault::volume::Pkcs5Prf::sha512 &&
                options.pim == 0 && !options.read_only &&
                !options.notify_shell,
            "created mount received the wrong fixed options");
    last_mount_password.assign(volume_password.bytes().begin(),
                               volume_password.bytes().end());
    last_mount_label = options.label;
    attached_volume_sizes[options.drive_letter] = volume_size_of(path);
    if (options.hidden_volume_protection.has_value()) {
      events.emplace_back("protect");
      const SecureBuffer& protection =
          *options.hidden_volume_protection->password;
      require(is_b_derivation(protection.bytes(), "login-password"),
              "a protected mount did not protect with the session derivation");
      last_protection_password.assign(protection.bytes().begin(),
                                      protection.bytes().end());
      // One rejection covers a password that opens no outer volume and a
      // hidden header that does not answer.
      const bool outer_here =
          std::ranges::find(protected_outer_paths, std::wstring(path)) !=
          protected_outer_paths.end();
      if (!outer_here ||
          !is_b_derivation(volume_password.bytes(), protected_outer_input) ||
          !protection_password_opens) {
        throw std::system_error(
            usb_vault::volume::make_error_code(
                usb_vault::volume::DriverError::authentication_failed),
            "wrong password");
      }
      attached_volume_types[options.drive_letter] = protected_volume_type;
      identities[options.drive_letter] = {.drive_letter=options.drive_letter,
          .unique_id=next_mount_id++, .volume_path=std::wstring(path),
          .disk_length=attached_volume_sizes[options.drive_letter],
          .protected_outer_writable=protected_volume_type==usb_vault::volume::VolumeType::outer};
      return {.drive_letter = options.drive_letter};
    }
    if (!login_derived_path.empty() && path == login_derived_path &&
        (nothing_opens_it || !derived_from_login(volume_password))) {
      // Exactly what the driver reports for a header that no candidate
      // password unlocks, which is the only signal the session gets before it
      // tries the other derivation.
      throw std::system_error(
          usb_vault::volume::make_error_code(
              usb_vault::volume::DriverError::authentication_failed),
          "wrong password");
    }
    if (fail_created_mount) throw std::runtime_error("mount created failed");
    identities[options.drive_letter] = {.drive_letter=options.drive_letter,
        .unique_id=next_mount_id++, .volume_path=std::wstring(path),
        .disk_length=attached_volume_sizes[options.drive_letter]};
    return {.drive_letter = options.drive_letter};
  }

  void notify_mounted(const wchar_t drive_letter) override {
    events.emplace_back("notify:A");
    // The shell is told about the drive the volume actually came up on, which
    // is the allocated one unless this driver was told to land it elsewhere.
    require(drive_letter ==
                (partition_lands_on ? partition_lands_on : allocated_drive_letter),
            "the shell was told about a drive the volume is not on");
    if (fail_notify) throw std::runtime_error("notify failed");
  }

  [[nodiscard]] wchar_t allocate_drive_letter() override {
    events.emplace_back("allocate");
    if (fail_allocate) throw std::runtime_error("no free drive letter");
    require(!free_drive_letters.empty(), "the fake ran out of drive letters");
    allocated_drive_letter = free_drive_letters.front();
    free_drive_letters.erase(free_drive_letters.begin());
    return allocated_drive_letter;
  }

  // The index file, held in memory. It starts as a real one does: every slot
  // filler, so nothing opens and nothing is remembered yet.
  [[nodiscard]] std::vector<std::byte> read_container_index() override {
    ++index_reads;
    if (fail_index_read) throw std::runtime_error("the index cannot be read");
    if (container_index.empty()) {
      container_index.resize(kContainerIndexV1FileSize);
      for (std::size_t slot = 0; slot < kContainerIndexV1SlotCount; ++slot) {
        fill_container_index_slot_v1(
            container_index_v1_mutable_slot(container_index, slot));
      }
    }
    return container_index;
  }

  void write_container_index(
      const std::span<const std::byte> contents) override {
    ++index_writes;
    if (fail_index_write) throw std::runtime_error("the index cannot be written");
    container_index.assign(contents.begin(), contents.end());
  }

  [[nodiscard]] bool remember_container_paths() override {
    return remember_paths;
  }

  void set_remember_container_paths(const bool remember) override {
    remember_paths = remember;
    ++remember_paths_writes;
  }

  std::vector<std::byte> container_index;
  int index_reads{};
  int index_writes{};
  bool fail_index_read{};
  bool fail_index_write{};
  [[nodiscard]] bool mount_imported_containers() override {
    return mount_imported;
  }

  void set_mount_imported_containers(const bool mount) override {
    mount_imported = mount;
    ++mount_imported_writes;
  }

  // The storage layer, recorded rather than performed. What the session is
  // answerable for is the absolute path each call arrives with, and that is
  // exactly what these keep.
  [[nodiscard]] VolumeDirectoryListing list_directory(
      const std::wstring_view absolute_directory) override {
    listed.emplace_back(absolute_directory);
    return listing;
  }

  void delete_entry(const std::wstring_view absolute_path) override {
    deleted.emplace_back(absolute_path);
  }

  void create_directory(const std::wstring_view absolute_path) override {
    created_directories.emplace_back(absolute_path);
  }

  void move_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination) override {
    moved.emplace_back(std::wstring(absolute_source),
                       std::wstring(absolute_destination));
  }

  void move_entry_by_copying(
      const std::wstring_view absolute_source,
      const std::wstring_view absolute_destination) override {
    moved_by_copying.emplace_back(std::wstring(absolute_source),
                                  std::wstring(absolute_destination));
  }

  void copy_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination,
                  const usb_vault::volume::TransferCancellation& cancelled) override {
    // What the real storage layer reports when a cancellation stops a copy.
    if (cancelled && cancelled()) {
      throw std::system_error(std::make_error_code(
          std::errc::operation_canceled));
    }
    copied.emplace_back(std::wstring(absolute_source),
                        std::wstring(absolute_destination));
    if (fail_copy) throw std::runtime_error("the copy failed");
  }

  void export_selection(std::span<const usb_vault::backup::ExportSelectionSourceV1> sources,
      std::wstring_view destination, std::wstring_view folder, bool encrypted,
      const SecureBuffer&, const std::function<bool()>&,
      const std::function<void(std::uint64_t, std::uint64_t)>&) override {
    selected_sources.assign(sources.begin(), sources.end());
    selected_destination = destination; selected_folder = folder; selected_encrypted = encrypted;
  }
  std::vector<usb_vault::backup::ExportSelectionSourceV1> selected_sources;
  std::wstring selected_destination, selected_folder;
  bool selected_encrypted{};
  void export_encrypted_entry(const std::wstring_view source,
                              const std::wstring_view destination,
                              const usb_vault::security::SecureBuffer& password,
                              const std::function<bool()>& cancelled,
                              const std::function<void(std::uint64_t, std::uint64_t)>&) override {
    if (cancelled && cancelled()) {
      throw std::system_error(ERROR_CANCELLED, std::system_category());
    }
    encrypted_exports.push_back({std::wstring(source), std::wstring(destination), password.size()});
    if (fail_copy) throw std::runtime_error("the encrypted export failed");
  }

  [[nodiscard]] std::size_t storage_calls() const noexcept {
    return listed.size() + deleted.size() + created_directories.size() +
           moved.size() + moved_by_copying.size() + copied.size() +
           encrypted_exports.size();
  }

  std::wstring import_package(const std::wstring_view package,
                              const usb_vault::security::SecureBuffer& password,
                              const wchar_t drive_letter,
                              const std::wstring_view relative_directory,
                              const std::function<bool()>&,
                              const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) override {
    imports.push_back({std::wstring(package),
                       std::wstring(1, drive_letter) + L"|" + std::wstring(relative_directory),
                       password.size()});
    if (fail_copy) throw std::runtime_error("the import failed");
    return L"Docs";
  }

  std::wstring write_import(usb_vault::backup::ImportPackageV1&,
                            const usb_vault::security::SecureBuffer&,
                            const wchar_t drive_letter,
                            const std::wstring_view relative_directory,
                            const std::function<bool()>&, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) override {
    import_writes.push_back(std::wstring(1, drive_letter) + L"|" + std::wstring(relative_directory));
    if (fail_copy) throw std::runtime_error("the import write failed");
    return L"Docs";
  }

  void delete_container_file(const std::wstring_view path) override {
    deleted_containers.emplace_back(path);
  }
  std::vector<std::wstring> import_writes;
  std::vector<std::wstring> deleted_containers;
  std::vector<std::wstring> protected_cleanups;
  void remove_protected_directory_if_empty(const std::wstring_view path) override {
    protected_cleanups.emplace_back(path);
  }

  struct EncryptedExport final {
    std::wstring source;
    std::wstring destination;
    std::size_t password_bytes{};
  };
  std::vector<EncryptedExport> encrypted_exports;
  std::vector<EncryptedExport> imports;

  VolumeDirectoryListing listing;
  bool fail_copy{};
  std::vector<std::wstring> listed;
  std::vector<std::wstring> deleted;
  std::vector<std::wstring> created_directories;
  std::vector<std::pair<std::wstring, std::wstring>> moved;
  std::vector<std::pair<std::wstring, std::wstring>> moved_by_copying;
  std::vector<std::pair<std::wstring, std::wstring>> copied;

  bool remember_paths{true};
  int remember_paths_writes{};
  bool mount_imported{};
  int mount_imported_writes{};

  wchar_t allocated_drive_letter{L'X'};
  // A container whose volumes were built from the login input. Only the B
  // derivation opens it, exactly as on a real hidden-volume container. Set
  // `nothing_opens_it` alongside it for a file that is not one of ours.
  std::wstring login_derived_path;
  bool nothing_opens_it{};
  // What the driver does with a protected mount: whose outer volume opens,
  // whether the protection password reaches a hidden header, what it reports.
  std::vector<std::wstring> protected_outer_paths;
  std::string protected_outer_input{"decoy-password"};
  bool protection_password_opens{true};
  usb_vault::volume::VolumeType protected_volume_type{
      usb_vault::volume::VolumeType::outer};
  std::vector<std::byte> last_protection_password;
  std::map<wchar_t, usb_vault::volume::VolumeType> attached_volume_types;
  std::uint64_t reported_total{32ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint64_t reported_free{9ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint64_t reported_safe{5ULL * 1024ULL * 1024ULL * 1024ULL};
  wchar_t space_drive{};
  wchar_t measured_drive{};
  unsigned int measure_calls{};
  unsigned int end_measure_calls{};
  bool fail_measure{};
  std::map<wchar_t, std::uint64_t> attached_volume_sizes;
  std::map<std::wstring, std::uint64_t> hidden_volume_sizes;
  std::vector<std::wstring> hidden_target_paths;

 private:
  [[nodiscard]] std::uint64_t volume_size_of(
      const std::wstring_view path) const {
    if (path == L"C:\\vault\\a.hc") return container_a_size_bytes;
    const auto hidden = hidden_volume_sizes.find(std::wstring(path));
    if (hidden != hidden_volume_sizes.end()) return hidden->second;
    return other_size_bytes;
  }

  [[nodiscard]] static bool derived_from_login(
      const SecureBuffer& volume_password) noexcept {
    constexpr std::string_view prefix = "derived-b-";
    if (volume_password.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (volume_password.bytes()[index] !=
          static_cast<std::byte>(prefix[index])) {
        return false;
      }
    }
    return true;
  }
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

// 🔴 `std::invalid_argument` is itself a `std::logic_error`, so asking only
// for the base accepts either one. The two mean different things at this
// boundary -- "no container answers to that" against "that container is not
// mounted" -- and a page has to be able to say which, so the tests insist on
// the difference rather than letting the hierarchy blur it.
template <typename Action>
void require_throws_plain_logic_error(Action&& action,
                                      const char* const message) {
  try {
    action();
  } catch (const std::invalid_argument&) {
    throw std::runtime_error(message);
  } catch (const std::logic_error&) {
    return;
  }
  throw std::runtime_error(message);
}

void require_events(const FakeRuntime& runtime,
                    const std::initializer_list<std::string_view> expected) {
  bool matched = runtime.events.size() == expected.size();
  std::size_t index = 0;
  for (const std::string_view event : expected) {
    if (!matched) break;
    matched = runtime.events[index] == event;
    ++index;
  }
  if (!matched) {
    std::string observed;
    for (const std::string& event : runtime.events) {
      if (!observed.empty()) observed += ", ";
      observed += event;
    }
    throw std::runtime_error("runtime event sequence differed; observed: " +
                             observed);
  }
}

void require_operation(const OperationEvent& event,
                       const std::uint64_t sequence,
                       const OperationKind kind,
                       const OperationOutcome outcome,
                       const std::wstring_view container_name,
                       const wchar_t drive_letter,
                       const char* const message) {
  require(
      event.sequence == sequence && event.completed_at_unix_ms > 0 &&
          event.kind == kind && event.outcome == outcome &&
          event.container_name == container_name &&
          event.container_name.find_first_of(L"\\/") == std::wstring::npos &&
          event.drive_letter == drive_letter,
      message);
}

void test_all_container_shapes_establish_authentication() {
  for (const auto [has_a, has_b] : {
           std::pair{false, false}, std::pair{true, false},
           std::pair{false, true}, std::pair{true, true}}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.container_a_exists = has_a;
    if (has_b) runtime.other_existing.emplace_back(L"C:\\vault\\b.hc");
    CardLoginMountSessionCore session(authentication, runtime, layout());
    SecureBuffer root = root_secret();
    SecureBuffer password = bytes("login-password");

    session.consume_authenticated_login(std::move(root),
                                        std::move(password));

    require(root.empty() && password.empty(),
            "successful handoff copied an input owner");
    require(authentication.authenticated() && !session.mounted(),
            "a container shape changed authentication or mounted at login");
    require(session.container_statuses().size() ==
                static_cast<std::size_t>(has_a) + static_cast<std::size_t>(has_b),
            "a container shape was not adopted at login");
    require_events(runtime, {"exists:final-other", "exists:final"});
  }
}

void test_mount_failures_do_not_revoke_card_authentication() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.container_a_exists = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.container_statuses()[0].id;
  runtime.fail_created_mount = true;
  runtime.events.clear();

  require_throws<std::runtime_error>(
      [&] { session.mount_adopted_container(id); },
      "ordinary mount failure was not propagated");
  require(authentication.authenticated() && !session.mounted(),
          "post-authentication mount failure revoked authentication or "
          "invented mount state");
  require_events(runtime, {"exists:final", "derive:A", "allocate",
                           "mount-created:A"});
}

void test_invalid_or_duplicate_handoff_fails_before_mount() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  require_throws<std::invalid_argument>(
      [&] {
        session.consume_authenticated_login(
            root_secret(usb_vault::card::kCardRecordV1RootSecretSize - 1),
            bytes("login-password"));
      },
      "invalid root was accepted");
  require(!authentication.authenticated() && runtime.events.empty(),
          "invalid root reached mount or established authentication");

  authentication.establish(root_secret());
  require_throws<std::logic_error>(
      [&] {
        session.consume_authenticated_login(root_secret(),
                                            bytes("login-password"));
      },
      "duplicate authenticated handoff was accepted");
  require(runtime.events.empty(),
          "duplicate authenticated handoff reached mount");
}

// Logs in with the product's manual policy and mounts the hidden container
// and then the standard one on the layout's own letters, which is where the
// tests below start from.
void login_and_mount_both(CardLoginMountSessionCore& session,
                          FakeRuntime& runtime) {
  runtime.container_a_exists = true;
  runtime.other_existing.emplace_back(L"C:\\vault\\b.hc");
  if (runtime.other_size_bytes == 0) {
    runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  }
  runtime.free_drive_letters = {L'Y', L'X'};
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  for (const ContainerStatus& status : session.container_statuses()) {
    session.mount_adopted_container(status.id);
  }
  const std::vector<ContainerStatus> mounted = session.container_statuses();
  require(mounted.size() == 2 && mounted[0].name == L"b.hc" &&
              mounted[0].drive_letter == L'Y' && mounted[1].name == L"a.hc" &&
              mounted[1].drive_letter == L'X',
          "test precondition did not mount B and then A on their letters");
  runtime.events.clear();
}

void test_unmount_state_is_precise_and_forced_order_is_b_then_a() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  login_and_mount_both(session, runtime);

  runtime.fail_unmount_b = true;
  require_throws<std::runtime_error>(
      [&] { session.unmount_active(); },
      "failed B unmount was hidden");
  require(session.mounted(), "failed B unmount lost active state");
  require_events(runtime, {"unmount:B"});

  runtime.fail_unmount_b = false;
  runtime.events.clear();
  runtime.force_required_a = true;
  require(session.force_unmount_active(),
          "required forced unmount was not reported");
  require(!session.mounted(), "forced unmount retained active state");
  require_events(runtime, {"force:B", "force:A"});
}

[[nodiscard]] const ContainerStatus* status_named(
    const std::vector<ContainerStatus>& statuses,
    const std::wstring_view name) {
  for (const ContainerStatus& status : statuses) {
    if (status.name == name) return &status;
  }
  return nullptr;
}

void test_card_removal_keeps_releasing_after_one_volume_fails() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  login_and_mount_both(session, runtime);

  // Hidden volume first. Its refusal must still let the standard volume go,
  // and the login password stays gone.
  runtime.events.clear();
  runtime.forced_unmount_attempts = 0;
  runtime.fail_manual_force_drive = L'Y';
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(session.force_unmount_active()); },
      "a volume that would not leave was reported as released");
  require(runtime.forced_unmount_attempts == 2,
          "card removal stopped after the first volume");
  require_events(runtime, {"force:A"});
  require(session.mounted() && session.copy_session_password().empty() &&
              session.operation_history().empty(),
          "card removal restored a secret or forgot the volume that stayed");
  const std::vector<ContainerStatus> hidden_first = session.container_statuses();
  const ContainerStatus* const hidden = status_named(hidden_first, L"b.hc");
  const ContainerStatus* const standard = status_named(hidden_first, L"a.hc");
  require(hidden != nullptr && hidden->mounted && hidden->drive_letter == L'Y' &&
              standard != nullptr && !standard->mounted,
          "the later volume stayed mounted after the earlier one refused");

  // The volume that stayed can still be released later.
  runtime.fail_manual_force_drive = 0;
  static_cast<void>(session.force_unmount_active());
  require(!session.mounted(), "the remaining volume could not be released");

  // A later refusal leaves the volume that already left down.
  CardAuthenticationSession again;
  FakeRuntime later;
  CardLoginMountSessionCore second(again, later, layout());
  login_and_mount_both(second, later);
  later.forced_unmount_attempts = 0;
  later.fail_manual_force_drive = L'X';
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(second.force_unmount_active()); },
      "a later refusal was hidden");
  require(later.forced_unmount_attempts == 2 &&
              later.events == std::vector<std::string>{"force:B"},
          "a later refusal undid the volume that had already left");
  const std::vector<ContainerStatus> standard_stuck = second.container_statuses();
  const ContainerStatus* const still_hidden = status_named(standard_stuck, L"b.hc");
  const ContainerStatus* const still_standard = status_named(standard_stuck, L"a.hc");
  require(still_hidden != nullptr && !still_hidden->mounted &&
              still_standard != nullptr && still_standard->mounted &&
              still_standard->drive_letter == L'X' &&
              second.copy_session_password().empty(),
          "the earlier volume was mounted again or the password came back");

  // Two ordinary volumes share one release group. The second is still asked.
  CardAuthenticationSession plain;
  FakeRuntime files;
  files.free_drive_letters = {L'X', L'W'};
  CardLoginMountSessionCore created(plain, files, layout());
  created.consume_authenticated_login(root_secret(), bytes("login-password"));
  require(plain.authenticated() && !created.mounted(),
          "test precondition did not establish an empty session");
  files.events.clear();
  static_cast<void>(created.create_container(L"C:\\vault\\a.hc",
                                             64ULL * 1024ULL * 1024ULL));
  static_cast<void>(created.create_container(L"C:\\vault\\second.hc",
                                             64ULL * 1024ULL * 1024ULL));
  const std::vector<ContainerStatus> created_statuses = created.container_statuses();
  const ContainerStatus* const newer = status_named(created_statuses, L"second.hc");
  const ContainerStatus* const older = status_named(created_statuses, L"a.hc");
  require(newer != nullptr && older != nullptr && newer->mounted && older->mounted,
          "the two ordinary volumes were not both mounted");
  files.events.clear();
  files.forced_unmount_attempts = 0;
  files.fail_manual_force_drive = newer->drive_letter;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(created.force_unmount_active()); },
      "one ordinary volume stopped the other");
  require(files.forced_unmount_attempts == 2,
          "card removal stopped inside one release group");
  const std::vector<ContainerStatus> after = created.container_statuses();
  const ContainerStatus* const newer_after = status_named(after, L"second.hc");
  const ContainerStatus* const older_after = status_named(after, L"a.hc");
  require(newer_after != nullptr && newer_after->mounted &&
              older_after != nullptr && !older_after->mounted &&
              created.copy_session_password().empty(),
          "the other ordinary volume stayed mounted or the password came back");
}

void establish_empty_authenticated_session(
    CardAuthenticationSession& authentication,
    FakeRuntime& runtime,
    CardLoginMountSessionCore& session) {
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  require(authentication.authenticated() && !session.mounted(),
          "test precondition did not establish an empty session");
  runtime.events.clear();
}

void test_container_a_creation_requires_authentication_and_valid_size() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  require_throws<std::logic_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "unauthenticated creation was accepted");
  require(runtime.events.empty(),
          "unauthenticated creation reached the runtime");

  establish_empty_authenticated_session(authentication, runtime, session);
  require_throws<std::invalid_argument>(
      [&] { session.create_container_a(513); },
      "unaligned creation size was accepted");
  require(runtime.events.empty(), "invalid size reached the runtime");
}

void test_file_hiding_publication_and_session_guards() {
  for (bool pair : {false, true}) for (bool fail : {false, true}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    require_throws<std::logic_error>([&] { session.set_container_file_hidden(1, true); }, "unauthenticated visibility accepted");
    establish_empty_authenticated_session(authentication, runtime, session);
    runtime.hiding_enabled = true;
    runtime.hiding_fails = fail;
    const auto create = [&] {
      return pair ? session.create_container_b(L"C:\\vault\\b.hc", 64ULL*1024*1024, 16ULL*1024*1024, bytes("decoy-password")) :
                    session.create_container(L"C:\\vault\\plain.hc", 64ULL*1024*1024);
    };
    if (fail) {
      require_throws<std::runtime_error>([&] { create(); }, "failed attributes published a container");
      require(runtime.other_staging.empty() && runtime.other_existing.empty() && !session.mounted(), "failed attributes did not clean staging");
      require(std::ranges::find(runtime.events, "publish:other") == runtime.events.end(), "failed attributes reached publish");
    } else {
      const auto id = create();
      require(runtime.visibility_path.ends_with(L".preparing"), "attributes not applied to staging");
      require(std::ranges::find(runtime.events, "file-attributes") < std::ranges::find(runtime.events, "publish:other"), "attributes applied after publication");
      require_throws<std::logic_error>([&] { session.set_container_file_hidden(id, false); }, "mounted visibility mutation accepted");
      require_throws<std::invalid_argument>([&] { session.set_container_file_hidden(9999, true); }, "unknown id accepted");
      require(session.container_file_hidden(id), "mounted attribute readback failed");
      session.unmount_container(id);
      session.set_container_file_hidden(id, false);
      require(!session.container_file_hidden(id), "unhide attribute readback failed");
      require(!runtime.visibility_path.ends_with(L".preparing"), "existing file action used staging");
    }
  }
}

void test_container_a_creation_publishes_then_mounts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  establish_empty_authenticated_session(authentication, runtime, session);

  session.create_container_a(64ULL * 1024ULL * 1024ULL);

  require(authentication.authenticated() && session.mounted(),
          "successful A creation lost authentication or mount state");
  require(runtime.container_a_exists && !runtime.staging_exists,
          "successful A creation left the wrong published state");
  require_events(runtime,
                 {"exists:final", "exists:stage", "derive:A", "create:A",
                  "publish:A", "allocate", "mount-created:A", "notify:A"});

  require_throws<std::logic_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "mounted A accepted duplicate creation");
}

void test_container_a_creation_rejects_existing_targets() {
  for (const bool final_exists : {true, false}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    runtime.container_a_exists = final_exists;
    runtime.staging_exists = !final_exists;

    require_throws<std::system_error>(
        [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
        "existing creation target was accepted");
    require(!session.mounted() && authentication.authenticated(),
            "existing target changed session state");
    require_events(runtime, final_exists
                                ? std::initializer_list<std::string_view>{
                                      "exists:final"}
                                : std::initializer_list<std::string_view>{
                                      "exists:final", "exists:stage"});
  }
}

void test_container_a_creation_cleans_prepublication_failures() {
  for (const bool publish_failure : {false, true}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    runtime.fail_create = !publish_failure;
    runtime.fail_publish = publish_failure;

    require_throws<std::runtime_error>(
        [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
        "prepublication creation failure was hidden");
    require(!runtime.staging_exists && !runtime.container_a_exists &&
                !session.mounted() && authentication.authenticated(),
            "prepublication failure left state or revoked authentication");
    require_events(runtime,
                   publish_failure
                       ? std::initializer_list<std::string_view>{
                             "exists:final", "exists:stage", "derive:A",
                             "create:A", "publish:A", "cleanup:A"}
                       : std::initializer_list<std::string_view>{
                             "exists:final", "exists:stage", "derive:A",
                             "create:A"});
  }
}

void test_container_a_creation_reports_cleanup_failure() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  establish_empty_authenticated_session(authentication, runtime, session);
  runtime.fail_publish = true;
  runtime.fail_remove = true;

  try {
    session.create_container_a(64ULL * 1024ULL * 1024ULL);
    throw std::runtime_error("cleanup failure was not reported");
  } catch (const ContainerCreationCleanupError& error) {
    require(error.operation_failure() && error.cleanup_failure(),
            "cleanup error lost one of its causes");
  }
  require(runtime.staging_exists && !session.mounted() &&
              authentication.authenticated(),
          "cleanup failure did not preserve precise state");
}

[[nodiscard]] bool remembers(const FakeRuntime& runtime, std::wstring_view path);

void test_container_a_mount_failure_keeps_published_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  establish_empty_authenticated_session(authentication, runtime, session);
  runtime.fail_created_mount = true;

  require_throws<std::runtime_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "post-publication mount failure was hidden");
  require(runtime.container_a_exists && !runtime.staging_exists &&
              !session.mounted() && authentication.authenticated(),
          "post-publication mount failure deleted data or changed auth");
  const std::vector<ContainerStatus> kept = session.container_statuses();
  require(kept.size() == 1 && kept[0].name == L"a.hc" && !kept[0].mounted &&
              kept[0].drive_letter == wchar_t{} && remembers(runtime, L"C:\\vault\\a.hc"),
          "a published container was dropped from the list when its mount failed");
  require_events(runtime,
                 {"exists:final", "exists:stage", "derive:A", "create:A",
                  "publish:A", "allocate", "mount-created:A"});
}

void test_container_a_notify_failure_retains_mount_state() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  establish_empty_authenticated_session(authentication, runtime, session);
  runtime.fail_notify = true;

  require_throws<std::runtime_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "mount notification failure was hidden");
  require(session.mounted() && authentication.authenticated(),
          "notification failure lost a real mount");
  runtime.fail_notify = false;
  runtime.events.clear();
  session.unmount_active();
  require_events(runtime, {"unmount:A"});
}

[[nodiscard]] FakeRuntime runtime_with_both_containers() {
  FakeRuntime runtime;
  runtime.container_a_exists = true;
  runtime.container_a_size_bytes = 64ULL * 1024ULL * 1024ULL;
  runtime.other_existing.emplace_back(L"C:\\vault\\b.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  return runtime;
}

void test_manual_login_adopts_without_deriving_anything() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout());

  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));

  // Containers with a hidden volume live wherever the user put them, so login
  // cannot tell whether one exists and must not pay for a derivation that may
  // open nothing. Both derivations wait for the mount that needs them.
  require_events(runtime, {"exists:final-other", "exists:final"});
  require(!session.mounted(), "manual login mounted a container");
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].name == L"b.hc" &&
              statuses[1].name == L"a.hc" && !statuses[0].mounted &&
              !statuses[1].mounted,
          "manual login did not adopt both containers unmounted");
}

void test_adopted_containers_mount_without_further_input() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::vector<ContainerStatus> adopted = session.container_statuses();
  const std::uint32_t hidden_id = adopted[0].id;
  const std::uint32_t standard_id = adopted[1].id;

  runtime.events.clear();
  session.mount_adopted_container(hidden_id);
  require(runtime.last_mount_password ==
              bytes_vector("derived-b-login-password...."),
          "the hidden container was not opened with the captured password");
  require_events(runtime, {"exists:final-other", "derive:B", "allocate",
                           "mount-created:other", "notify:A"});

  runtime.events.clear();
  session.mount_adopted_container(standard_id);
  require(runtime.last_mount_password ==
              bytes_vector("derived-container-a-password"),
          "the standard container was not opened with the card derivation");
  require_events(runtime, {"exists:final", "derive:A", "allocate",
                           "mount-created:A", "notify:A"});

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].mounted && statuses[1].mounted &&
              statuses[0].drive_letter != statuses[1].drive_letter,
          "the adopted containers did not both mount on distinct drives");

  // Mounting an already mounted container is a no-op, and an unknown
  // identifier is refused.
  runtime.events.clear();
  session.mount_adopted_container(hidden_id);
  require(runtime.events.empty(), "a mounted container was mounted again");
  require_throws<std::invalid_argument>(
      [&] { session.mount_adopted_container(hidden_id + 100); },
      "an unknown identifier was accepted");
}

void test_forced_release_drops_the_captured_volume_password() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::uint32_t hidden_id = session.container_statuses()[0].id;
  session.mount_adopted_container(hidden_id);

  // Card removal releases the volumes and the captured password with them.
  static_cast<void>(session.force_unmount_active());
  require(session.operation_history().empty(),
          "card removal retained the closed session's operation history");
  require_throws<std::logic_error>(
      [&] { session.mount_adopted_container(hidden_id); },
      "the hidden container was mounted after its password was dropped");
}

void test_manual_forced_release_preserves_scope_secrets_and_failed_state() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  runtime.free_drive_letters = {L'Y', L'X', L'Y', L'Y'};
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const auto entries = session.container_statuses();
  const auto first = entries[0].id;
  const auto second = entries[1].id;
  session.mount_adopted_container(first);
  session.mount_adopted_container(second);
  const auto history_before = session.operation_history().size();
  runtime.fail_manual_force = true;
  require_throws<std::runtime_error>([&] { session.force_unmount_container(first); }, "force failure was swallowed");
  require(session.container_statuses()[0].mounted && session.container_statuses()[1].mounted,
          "failed force cleared active state");
  runtime.fail_manual_force = false;
  session.force_unmount_container(first);
  require(!session.container_statuses()[0].mounted && session.container_statuses()[1].mounted,
          "single force crossed target boundary");
  require(authentication.authenticated() && !session.copy_session_password().empty() &&
          session.operation_history().size() == history_before + 2,
          "manual force cleared authentication, password or history");
  session.mount_adopted_container(first);
  require(session.container_statuses()[0].mounted, "manual force prevented remount");
  runtime.events.clear();
  runtime.fail_manual_force_drive = L'X';
  require_throws<std::runtime_error>([&] { session.force_unmount_user_active(); }, "partial force failure was hidden");
  require(runtime.events == std::vector<std::string>{"force:B"}, "partial force changed release order");
  require(!session.container_statuses()[0].mounted && session.container_statuses()[1].mounted &&
          session.mounted(), "partial cleanup lost precise remaining state");
  runtime.fail_manual_force_drive = 0;
  runtime.events.clear();
  session.force_unmount_user_active();
  require(!session.mounted() && runtime.events == std::vector<std::string>{"force:A"},
          "unified manual force changed release order or left a volume");
  require_throws<std::invalid_argument>([&] { session.force_unmount_container(9999); }, "unknown force target accepted");
  session.mount_adopted_container(first);
  require(session.mounted(), "unified manual force destroyed remount secrets");
}

void test_operation_selection_does_not_query_attached_volume_capacity() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const auto before = session.container_operation_targets();
  require(before.size() == 2 && !before[0].mounted, "selection snapshot missing adopted items");
  session.mount_adopted_container(before[0].id);
  runtime.attached_volume_sizes.clear(); // An external disappearance must not block selecting the other item.
  runtime.events.clear();
  const auto targets = session.container_operation_targets();
  require(targets.size() == 2 && targets[0].id == before[0].id && targets[0].mounted &&
          targets[0].name == before[0].name && runtime.events.empty(),
          "selection performed driver IO or changed session facts");
  authentication.clear_authentication();
  require(session.container_operation_targets().empty(), "selection exposed names without authentication");
}

void test_deliberate_release_keeps_the_captured_volume_password() {
  // A deliberate unmount keeps it so the same session can mount again.
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::uint32_t hidden_id = session.container_statuses()[0].id;
  session.mount_adopted_container(hidden_id);
  session.unmount_active();
  session.mount_adopted_container(hidden_id);
  require(session.container_statuses()[0].mounted,
          "a deliberate unmount blocked a later mount in the same session");
}

// The runtime log follows a session through its stages and failures with
// their codes, and never carries a password or a derived key.
void test_runtime_log_records_session_stages_and_failures() {
#if USB_VAULT_DEBUG_LOG
  namespace diagnostics = usb_vault::diagnostics;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      (L"usb-vault-session-log-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
       std::to_wstring(::GetTickCount64()));
  require(std::filesystem::create_directory(root), "the log fixture directory already exists");
  const SYSTEMTIME time{2026, 9, 0, 26, 10, 0, 0, 0};
  const std::filesystem::path path = root / L"logs" / L"runtime-log-20260926-100000.txt";
  {
    diagnostics::DebugLogFile file;
    require(file.open(root, time), "cannot open the session log fixture");
    diagnostics::set_runtime_log(&file);
    struct Withdraw final {
      ~Withdraw() { usb_vault::diagnostics::set_runtime_log(nullptr); }
    } withdraw;

    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.remember_paths = true;
    runtime.free_drive_letters = {L'X', L'S', L'T', L'U'};
    runtime.other_existing = {L"D:\\kept.hc", L"D:\\kept2.hc"};
    runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
    runtime.login_derived_path = L"D:\\kept.hc";
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));

    session.create_container_a(64ULL * 1024ULL * 1024ULL);
    runtime.fail_unmount_a = true;
    require_throws<std::runtime_error>([&] { session.unmount_active(); },
                                       "a refused unmount was accepted");
    runtime.fail_unmount_a = false;
    static_cast<void>(session.mount_container(L"D:\\kept.hc"));
    runtime.fail_publish = true;
    runtime.fail_remove = true;
    require_throws<ContainerCreationCleanupError>(
        [&] {
          static_cast<void>(session.create_container(L"D:\\broken.hc",
                                                     64ULL * 1024ULL * 1024ULL));
        },
        "a failed cleanup was not reported");
    runtime.fail_publish = false;
    runtime.fail_remove = false;
    runtime.fail_index_write = true;
    static_cast<void>(session.adopt_container(L"D:\\kept2.hc"));
    runtime.fail_index_write = false;
    authentication.clear_authentication();
    static_cast<void>(session.force_unmount_active());
  }
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "the session log was not written");
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  for (const char* const expected : {
           "[会话] 登录完成；收录安全盘数 0",
           "[会话] 开始创建文件安全盘；路径 C:\\vault\\a.hc；暂存路径 C:\\vault\\a.hc.preparing；容量 67108864 字节",
           "[会话] 挂载新建安全盘；路径 C:\\vault\\a.hc；请求盘符 X:；PRF 1；PIM 0；只读 否；通知资源管理器 否；隐藏卷保护 否",
           "[会话] 新建安全盘挂载返回；驱动回报盘符 X:",
           "[会话] 文件安全盘创建完成；安全盘 #",
           "[会话] 全部卸载：卸载安全盘失败；安全盘 #",
           "异常类型：class std::runtime_error；原文：unmount A failed",
           "[会话] 标准派生挂载失败；路径 D:\\kept.hc；异常类型：class std::system_error；错误类别：",
           "错误码：3（0x00000003）",
           "[会话] 标准派生被拒，改用登录派生；路径 D:\\kept.hc",
           "[会话] 登录派生挂载成功；路径 D:\\kept.hc",
           "[会话] 清理暂存文件失败；暂存路径 D:\\broken.hc.preparing",
           "[会话] 创建文件安全盘失败；路径 D:\\broken.hc；暂存已生成 是；已发布 否",
           "[会话] 记录安全盘路径失败；路径 D:\\kept2.hc；异常类型：class std::runtime_error；原文：the index cannot be written",
           "[会话] 拔卡清理开始",
           "[会话] 拔卡清理结束；有卷被强制卸下"}) {
    if (text.find(expected) == std::string::npos) {
      throw std::runtime_error(std::string("the session log lacks: ") + expected);
    }
  }
  for (const char* const secret :
       {"login-password", "derived-b-", "derived-container-a-password", "diskA"}) {
    require(text.find(secret) == std::string::npos,
            "the session log carried a password or a derived key");
  }
  std::filesystem::remove(path);
  std::filesystem::remove(root / L"logs");
  std::filesystem::remove(root);
#endif
}

void test_import_falls_back_to_the_login_derivation() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  runtime.other_existing.emplace_back(L"D:\\kept.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  runtime.login_derived_path = L"D:\\kept.hc";
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  runtime.events.clear();

  // The user picks a file, never a container class, and the page offers no
  // such choice either. The standard derivation goes first and the login
  // derivation second, so both classes are reached through one entry point.
  const std::uint32_t id = session.mount_container(L"D:\\kept.hc");
  require_events(runtime,
                 {"exists:final-other", "derive:A", "allocate",
                  "mount-created:other", "derive:B", "mount-created:other",
                  "notify:A"});
  require(runtime.last_mount_password ==
              bytes_vector("derived-b-login-password...."),
          "the imported container was opened with the wrong derivation");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == id && statuses[0].mounted &&
              statuses[0].name == L"kept.hc",
          "the imported container was not adopted as mounted");

  // Adoption recorded which derivation opened it, so a later mount in this
  // session no longer probes the standard one.
  runtime.events.clear();
  session.unmount_container(id);
  session.mount_adopted_container(id);
  require_events(runtime, {"unmount:other", "exists:final-other", "allocate",
                           "mount-created:other", "notify:A"});
}

void test_import_reports_a_file_no_derivation_opens() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.other_existing.emplace_back(L"D:\\foreign.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  // Neither derivation opens it, so the second failure is the one reported.
  runtime.login_derived_path = L"D:\\foreign.hc";
  runtime.nothing_opens_it = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  runtime.events.clear();

  require_throws<std::system_error>(
      [&] {
        static_cast<void>(session.mount_container(L"D:\\foreign.hc"));
      },
      "a file no derivation opens was reported as mounted");
  require(session.container_statuses().empty(),
          "a failed import still adopted the file");
}

void test_container_b_creation_derives_both_volume_passwords() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  // The hidden volume password is derived from the input the session was
  // opened with, so the caller no longer supplies it.
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();

  const std::uint32_t id = session.create_container_b(
      L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL, bytes("decoy-password"));

  require(id != 0, "container B creation returned no identifier");
  require(runtime.outer_size == 64ULL * 1024ULL * 1024ULL &&
              runtime.hidden_size == 16ULL * 1024ULL * 1024ULL,
          "container B was created with the wrong sizes");
  require_events(runtime,
                 {"exists:final-other", "exists:stage-other", "derive:B",
                  "derive:B", "create:hidden", "publish:other", "allocate",
                  "mount-created:other", "notify:A"});

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == id &&
              statuses[0].mounted && statuses[0].name == L"b.hc",
          "container B was not adopted as a mounted container");
  require(session.mounted(), "container B creation left no active volume");

  // Creation keeps the derived volume password the login path would have
  // produced, so the container it just created can be released and opened
  // again without the user producing anything.
  runtime.events.clear();
  session.unmount_container(id);
  session.mount_adopted_container(id);
  require(session.container_statuses()[0].mounted,
          "a container created in this session could not be mounted again");
}

void test_container_b_creation_rejects_unusable_input() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "unauthenticated container B creation was accepted");

  // Authentication alone is not enough: the derivation needs the input the
  // session was opened with, and a session that never captured one refuses
  // instead of inventing a substitute.
  CardAuthenticationSession bare_authentication;
  FakeRuntime bare_runtime;
  const auto bare = std::make_unique<CardLoginMountSessionCore>(
      bare_authentication, bare_runtime, layout());
  bare_authentication.establish(root_secret());
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(bare->create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "container B was created without the input the session captured");
  require(bare_runtime.events.empty(),
          "a session without a captured login still reached the runtime");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();
  for (const auto [outer, hidden] : {
           std::pair<std::uint64_t, std::uint64_t>{0, 0},
           std::pair<std::uint64_t, std::uint64_t>{1024, 0},
           std::pair<std::uint64_t, std::uint64_t>{513, 0},
           std::pair<std::uint64_t, std::uint64_t>{1024, 513},
           std::pair<std::uint64_t, std::uint64_t>{1024, 1024}}) {
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(session.create_container_b(
              L"C:\\vault\\b.hc", outer, hidden,
              bytes("decoy-password")));
        },
        "container B accepted an unusable size pair");
  }
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(session.create_container_b(L"C:\\vault\\b.hc",
                                                     1024, 512,
                                                     SecureBuffer()));
      },
      "container B accepted an empty decoy password");
  require(runtime.events.empty(),
          "a rejected container B request still reached the runtime");
}

void test_container_b_creation_cleans_a_failed_format() {
  FakeRuntime runtime;
  runtime.fail_create = true;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();

  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "a failed container B format was not reported");
  require(session.container_statuses().empty() && !session.mounted(),
          "a failed container B creation left session state behind");
}

void test_hidden_containers_are_unlimited_and_share_one_derivation() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::uint32_t first = session.create_container_b(
      L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  runtime.events.clear();
  const std::uint32_t second = session.create_container_b(
      L"E:\\two.hc", 128ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));

  require(first != 0 && second != 0 && first != second,
          "the two containers did not get distinct identifiers");
  // Every container of this kind shares one pair of passwords, so the second
  // creation derives only the decoy side: the login side is still held.
  require_events(runtime,
                 {"exists:final-other", "exists:stage-other", "derive:B",
                  "create:hidden", "publish:other", "allocate",
                  "mount-created:other", "notify:A"});
  require(runtime.hidden_target_paths.size() == 2 &&
              runtime.hidden_target_paths[0] == L"D:\\one.hc.preparing" &&
              runtime.hidden_target_paths[1] == L"E:\\two.hc.preparing",
          "the containers were not created at the caller's own paths");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].name == L"two.hc" &&
              statuses[1].name == L"one.hc" && statuses[0].mounted &&
              statuses[1].mounted &&
              statuses[0].drive_letter != statuses[1].drive_letter,
          "the session did not adopt both containers on distinct drives");

  // A path already adopted is still refused, which is the only count-like
  // limit left.
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "the same path was accepted twice");
}

void test_capacity_reports_the_attached_volume_not_the_file() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  // A container holding a hidden volume is larger than the volume the user
  // actually gets, so the file size would misreport it by 32 MiB here.
  const std::uint32_t hidden = session.create_container_b(
      L"D:\\two-layer.hc", 128ULL * 1024ULL * 1024ULL,
      96ULL * 1024ULL * 1024ULL, bytes("decoy-password"));
  static_cast<void>(session.create_container(L"D:\\plain.hc",
                                             64ULL * 1024ULL * 1024ULL));

  std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 &&
              statuses[1].size_bytes == 96ULL * 1024ULL * 1024ULL &&
              statuses[0].size_bytes == 64ULL * 1024ULL * 1024ULL,
          "capacity did not follow the volume the driver attached");

  session.unmount_container(hidden);
  const wchar_t standard_drive = statuses[0].drive_letter;
  statuses = session.container_statuses();
  require(statuses.size() == 2 && !statuses[1].mounted &&
              statuses[1].size_bytes == 0 &&
              statuses[1].drive_letter == wchar_t{} &&
              statuses[0].size_bytes == 64ULL * 1024ULL * 1024ULL,
          "an unmounted container still reported a capacity");

  // A container this session believes is mounted, on a drive the driver
  // reports empty, was released outside the application.
  runtime.attached_volume_sizes.erase(standard_drive);
  statuses = session.container_statuses();
  require(statuses.size() == 2 && !statuses[0].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              !statuses[0].volume_unavailable && !session.mounted(),
          "a volume released outside the application still read as mounted");
}

void test_one_unreadable_volume_does_not_hide_the_list() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U'};
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t first = session.create_container(
      L"D:\\first.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t second = session.create_container(
      L"D:\\second.hc", 64ULL * 1024ULL * 1024ULL);
  std::vector<ContainerStatus> statuses = session.container_statuses();
  const wchar_t first_drive = status_named(statuses, L"first.hc")->drive_letter;
  const wchar_t second_drive = status_named(statuses, L"second.hc")->drive_letter;

  // The driver cannot answer for one drive. That row keeps its mount and its
  // letter, and every other row is reported as usual.
  runtime.fail_volume_size_drive = first_drive;
  statuses = session.container_statuses();
  const ContainerStatus* unreadable = status_named(statuses, L"first.hc");
  const ContainerStatus* readable = status_named(statuses, L"second.hc");
  require(statuses.size() == 2 && unreadable != nullptr && readable != nullptr &&
              unreadable->mounted && unreadable->volume_unavailable &&
              unreadable->size_bytes == 0 &&
              unreadable->drive_letter == first_drive &&
              readable->mounted && !readable->volume_unavailable &&
              readable->size_bytes == 64ULL * 1024ULL * 1024ULL,
          "one unreadable volume hid or misreported the rest of the list");

  // The mount was not disproved, so it can still be taken down.
  session.unmount_container(first);
  runtime.fail_volume_size_drive = wchar_t{};
  require(!status_named(session.container_statuses(), L"first.hc")->mounted,
          "an unreadable row could not be unmounted");

  // A volume released outside the application reads as unmounted and can be
  // mounted again.
  runtime.attached_volume_sizes.erase(second_drive);
  statuses = session.container_statuses();
  require(!status_named(statuses, L"second.hc")->mounted && !session.mounted(),
          "an externally released volume kept its mounted row");
  session.mount_adopted_container(second);
  require(status_named(session.container_statuses(), L"second.hc")->mounted,
          "an externally released container could not be mounted again");
}

void test_container_a_status_projects_live_file_and_mount_facts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  establish_empty_authenticated_session(authentication, runtime, session);

  const auto absent = session.container_a_status();
  require(!absent.exists && !absent.mounted && absent.size_bytes == 0 &&
              absent.drive_letter == wchar_t{},
          "absent A status exposed invented metadata");
  require_events(runtime, {"size:A"});

  runtime.events.clear();
  runtime.container_a_exists = true;
  runtime.container_a_size_bytes = 96ULL * 1024ULL * 1024ULL;
  const auto present = session.container_a_status();
  require(present.exists && !present.mounted &&
              present.size_bytes == 96ULL * 1024ULL * 1024ULL &&
              present.drive_letter == wchar_t{},
          "unmounted A status lost live capacity or invented a drive");
  require_events(runtime, {"size:A"});

  CardAuthenticationSession mounted_authentication;
  FakeRuntime mounted_runtime;
  mounted_runtime.container_a_exists = true;
  const auto mounted_session = std::make_unique<CardLoginMountSessionCore>(
      mounted_authentication, mounted_runtime, layout());
  mounted_session->consume_authenticated_login(root_secret(),
                                               bytes("login-password"));
  mounted_session->mount_adopted_container(
      mounted_session->container_statuses()[0].id);
  mounted_runtime.events.clear();
  const auto mounted = mounted_session->container_a_status();
  require(mounted.exists && mounted.mounted &&
              mounted.size_bytes == 64ULL * 1024ULL * 1024ULL &&
              mounted.drive_letter == L'X',
          "mounted A status did not project its fixed drive and capacity");

  mounted_runtime.container_a_exists = false;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(mounted_session->container_a_status()); },
      "missing backing file was hidden for a recorded A mount");
}

void test_login_authenticates_without_mounting() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.container_a_exists = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  require(authentication.authenticated(),
          "login did not establish card authentication");
  // Login only looks for the fixed containers so the user can mount them
  // later; it must not mount anything by itself.
  require_events(runtime, {"exists:final-other", "exists:final"});
  require(!session.mounted(),
          "login reported an active volume");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].name == L"a.hc" &&
              !statuses[0].mounted && statuses[0].drive_letter == wchar_t{},
          "login did not adopt the existing container unmounted");

  const auto status = session.container_a_status();
  require(status.exists && !status.mounted && status.drive_letter == wchar_t{},
          "an existing container was projected as mounted after login");
}

void test_initialization_handoff_supports_hidden_creation() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  require_throws<std::logic_error>([&] { session.complete_initialization(bytes("new-password")); },
      "initialization handoff accepted an unauthenticated session");
  authentication.establish(root_secret());
  require_throws<std::logic_error>([&] { session.complete_initialization(SecureBuffer()); },
      "initialization handoff accepted an empty password");
  session.complete_initialization(bytes("new-password"));
  require(runtime.events.empty() && !session.mounted(), "initialization handoff touched storage or mounted");
  const auto expected = bytes("new-password");
  const auto retained = session.copy_session_password();
  require(std::ranges::equal(expected.bytes(), retained.bytes()), "initialization password was not retained");
  require_throws<std::logic_error>([&] { session.complete_initialization(bytes("replacement")); },
      "initialization handoff replaced an existing session password");
  const auto id = session.create_container_b(L"C:\\vault\\fresh.hc", 64ULL * 1024 * 1024,
      16ULL * 1024 * 1024, bytes("decoy-password"));
  require(id != 0 && session.mounted(), "hidden creation after initialization did not mount");
  session.unmount_active();
  session.release_session_secrets();
  require(session.copy_session_password().empty(), "initialization password survived session cleanup");
}

void test_manual_policy_still_serializes_and_allows_creation() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  require_throws<std::logic_error>(
      [&] {
        session.consume_authenticated_login(root_secret(),
                                            bytes("login-password"));
      },
      "a second handoff was accepted after a manual login");

  session.create_container_a(64ULL * 1024ULL * 1024ULL);
  require(session.mounted(),
          "creation after a manual login did not mount the new container");
  const auto status = session.container_a_status();
  require(status.exists && status.mounted && status.drive_letter == L'X',
          "creation after a manual login did not project the mounted volume");
}

void test_multiple_standard_containers_get_distinct_drive_letters() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::uint32_t first = session.create_container(
      layout().container_a.path, 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t second = session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL);
  require(first != second, "two containers were adopted under one identifier");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2, "the session did not adopt both containers");
  require(statuses[0].id == second && statuses[1].id == first,
          "new container did not go first");
  require(statuses[0].name == L"second.hc" && statuses[1].name == L"a.hc",
          "container statuses did not project the file name");
  require(statuses[0].drive_letter == L'W' &&
              statuses[1].drive_letter == L'X',
          "the two containers did not receive distinct drive letters");
  require(statuses[0].mounted && statuses[1].mounted &&
              statuses[0].size_bytes == 64ULL * 1024ULL * 1024ULL &&
              statuses[1].size_bytes == 64ULL * 1024ULL * 1024ULL,
          "created containers were not projected as mounted with capacity");
  require(session.mounted(), "the session reported no active volume");
}

void test_container_paths_are_adopted_once() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));

  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));
      },
      "an adopted container path was created a second time");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\second.hc")); },
      "an already mounted container was mounted again");
  require(session.container_statuses().size() == 1,
          "a rejected creation still changed the adopted collection");
}

void test_unmounted_container_can_be_mounted_again() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL);

  session.unmount_container(id);
  require(!session.mounted(), "unmounting one container left the session active");
  std::vector<ContainerStatus> statuses = session.container_statuses();
  // Capacity describes the attached volume, so an unmounted container reports
  // none of it - exactly as it reports no drive letter.
  require(statuses.size() == 1 && !statuses[0].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              statuses[0].size_bytes == 0,
          "an unmounted container was not retained without a capacity");

  const std::uint32_t again = session.mount_container(L"C:\\vault\\second.hc");
  require(again == id, "remounting did not reuse the adopted identifier");
  statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].mounted &&
              statuses[0].drive_letter == L'W',
          "remounting did not allocate a fresh drive letter");

  session.unmount_container(id);
  session.unmount_container(id);
  require_throws<std::invalid_argument>(
      [&] { session.unmount_container(id + 100); },
      "an unknown container identifier was accepted");
  require(runtime.batch_unmounts == 0 && runtime.batch_forced_unmounts == 0,
          "a single unmount took the batch path");

}

void test_batch_items_take_the_batch_unmount_path() {
  for (const bool force : {false, true}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    const std::uint32_t id = session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL);
    session.unmount_container_impl(id, force, true, true);
    require(!session.mounted() && runtime.batch_unmounts == int(!force) &&
                runtime.batch_forced_unmounts == int(force),
            "a batch item did not take the batch unmount path");
  }
}

void test_mount_requires_authentication_and_an_existing_file() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\second.hc")); },
      "mounting was accepted before authentication");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  require_throws<std::system_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\third.hc")); },
      "a missing container file was mounted");
  require(session.container_statuses().empty(),
          "a failed mount still adopted the container");
}

void test_unmount_active_releases_every_adopted_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(session.create_container(layout().container_a.path,
                                             64ULL * 1024ULL * 1024ULL));
  static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));

  session.unmount_active();

  require(!session.mounted(),
          "unmount_active left an adopted container mounted");
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && !statuses[0].mounted &&
              !statuses[1].mounted,
          "unmount_active dropped the adopted containers");
}

void test_creation_feedback_prevents_mount_after_interruption() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  std::vector<unsigned int> stages;
  runtime.creation_feedback = [&](unsigned int stage, unsigned int, wchar_t) {
    stages.push_back(stage);
    if (stage == 4) throw std::runtime_error("simulated card removal");
  };
  bool rejected = false;
  try { static_cast<void>(session.create_container(L"C:\\vault\\interrupted.hc", 64ULL * 1024ULL * 1024ULL)); }
  catch (...) { rejected = true; }
  require(rejected && !session.mounted() && runtime.other_staging.empty() &&
      runtime.other_existing.empty() && std::find(stages.begin(), stages.end(), 4U) != stages.end(),
      "interrupted creation published or mounted a container or retained staging");
}

void test_a_cancelled_creation_publishes_nothing_and_keeps_no_staging() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  // The worst case for a cancel: it is still pending when the helper has
  // already written the whole container. The file is under the staging name
  // until it is published, so the answer is still no container.
  runtime.creation_cancelled = [] { return true; };
  bool rejected = false;
  try {
    static_cast<void>(session.create_container(L"C:\\vault\\called-off.hc",
                                               64ULL * 1024ULL * 1024ULL));
  } catch (...) { rejected = true; }
  require(rejected && !session.mounted() && runtime.other_staging.empty() &&
      runtime.other_existing.empty(),
      "a cancelled creation published a container or kept its staging file");
}

void test_both_creation_paths_preserve_cancellation_cleanup_and_retry() {
  using usb_vault::volume_creation::CreationError;
  using usb_vault::volume_creation::make_error_code;
  constexpr std::wstring_view path = L"C:\\vault\\cancel-retry.hc";
  for (const bool hidden : {false, true}) {
    // Exercise both the session's post-format query and an abort from the
    // publication callback after that query has already returned false.
    for (const bool at_publication : {false, true}) {
      for (const bool cleanup_fails : {false, true}) {
        CardAuthenticationSession authentication;
        FakeRuntime runtime;
        CardLoginMountSessionCore session(authentication, runtime, layout());
        session.consume_authenticated_login(root_secret(), bytes("login-password"));
        const auto create = [&] {
          return hidden
              ? session.create_container_b(path, 64ULL * 1024 * 1024,
                    16ULL * 1024 * 1024, bytes("decoy-password"))
              : session.create_container(path, 64ULL * 1024 * 1024);
        };
        runtime.fail_remove = cleanup_fails;
        runtime.creation_cancelled = [=] { return !at_publication; };
        bool publication_boundary_seen = false;
        runtime.creation_feedback = [&](unsigned int stage, unsigned int, wchar_t) {
          if (stage == 4) {
            publication_boundary_seen = true;
            throw std::system_error(make_error_code(CreationError::operation_cancelled));
          }
        };
        runtime.events.clear();
        bool cancelled = false;
        bool cleanup_reported = false;
        try { static_cast<void>(create()); }
        catch (const ContainerCreationCleanupError& error) {
          cleanup_reported = error.cleanup_failure() != nullptr;
          try { std::rethrow_exception(error.operation_failure()); }
          catch (const std::system_error& cause) {
            cancelled = cause.code() == make_error_code(CreationError::operation_cancelled);
          }
        } catch (const std::system_error& error) {
          cancelled = error.code() == make_error_code(CreationError::operation_cancelled);
        }
        require(cancelled && cleanup_reported == cleanup_fails &&
            publication_boundary_seen == at_publication && authentication.authenticated() &&
            session.container_statuses().empty() && !session.mounted() &&
            runtime.other_existing.empty() &&
            runtime.other_staging.size() == (cleanup_fails ? 1U : 0U) &&
            std::count(runtime.events.begin(), runtime.events.end(), "cleanup:other") == 1 &&
            std::find(runtime.events.begin(), runtime.events.end(), "publish:other") == runtime.events.end() &&
            std::find(runtime.events.begin(), runtime.events.end(), "mount-created:other") == runtime.events.end(),
            "cancellation lost its cause, published a container, or misrepresented staging cleanup");
        runtime.creation_cancelled = {};
        runtime.creation_feedback = {};
        runtime.fail_remove = false;
        if (cleanup_fails) {
          // A residual staging file must block overwrite; recovery here
          // explicitly models external cleanup, never automatic deletion.
          const auto events_before_retry = runtime.events.size();
          require_throws<std::system_error>([&] { static_cast<void>(create()); },
              "same-name retry overwrote a residual staging file");
          require(runtime.other_staging.size() == 1 &&
              std::none_of(runtime.events.begin() + events_before_retry, runtime.events.end(),
                  [](const auto& event) { return event.starts_with("create:"); }),
              "blocked retry entered the formatter or removed its residual");
          runtime.remove_incomplete(std::wstring(path) + L".preparing");
        }
        const auto id = create();
        require(id != 0 && session.mounted() && session.container_statuses().size() == 1 &&
            runtime.other_existing == std::vector<std::wstring>{std::wstring(path)} &&
            runtime.other_staging.empty(),
            "same-session same-name retry failed after cancellation cleanup");
      }
    }
  }
}

void test_both_creation_paths_do_not_cancel_after_publication() {
  for (const bool hidden : {false, true}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    bool late_request = false;
    runtime.creation_cancelled = [&] { return late_request; };
    runtime.creation_feedback = [&](unsigned int stage, unsigned int, wchar_t) {
      if (stage == 5) late_request = true;
    };
    constexpr std::wstring_view path = L"C:\\vault\\already-published.hc";
    const auto id = hidden
        ? session.create_container_b(path, 64ULL * 1024 * 1024,
              16ULL * 1024 * 1024, bytes("decoy-password"))
        : session.create_container(path, 64ULL * 1024 * 1024);
    require(late_request && id != 0 && session.mounted() &&
        runtime.other_existing == std::vector<std::wstring>{std::wstring(path)} &&
        runtime.other_staging.empty() &&
        std::find(runtime.events.begin(), runtime.events.end(), "cleanup:other") == runtime.events.end(),
        "late cancellation undid publication or removed the completed container");
  }
}

void test_operation_logging_can_pause_without_affecting_volume_actions() {
  {
    FakeRuntime preference_runtime;
    CardAuthenticationSession preference_auth;
    CardLoginMountSessionCore original(preference_auth, preference_runtime, layout());
    original.set_operation_logging_enabled(false);
    original.set_force_unmount_enabled(true);
    original.set_auto_mount_enabled(true);
    CardLoginMountSessionCore reopened(preference_auth, preference_runtime, layout());
    require(!reopened.operation_logging_enabled() && reopened.force_unmount_enabled(), "new session lost saved preferences");
    require(reopened.auto_mount_enabled() && preference_runtime.events.empty(), "auto preference was lost or triggered volume work");
    preference_runtime.fail_preference_write = true;
    require_throws<std::runtime_error>([&] { reopened.set_auto_mount_enabled(false); }, "failed auto save was swallowed");
    require(reopened.auto_mount_enabled(), "failed auto save changed value");
    require_throws<std::runtime_error>([&] { reopened.set_operation_logging_enabled(true); }, "failed save was swallowed");
    require(!reopened.operation_logging_enabled(), "failed save changed logging state");
  }
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout());
  require(session.operation_logging_enabled(), "logging is not enabled by default");
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const auto before = session.operation_history().size();
  session.set_operation_logging_enabled(false);
  const auto id = session.create_container(L"C:\\vault\\work.hc",
                                           64ULL * 1024ULL * 1024ULL);
  session.unmount_container(id);
  require(session.operation_history().size() == before,
          "disabled logging recorded operations or removed existing history");
  session.set_operation_logging_enabled(true);
  session.mount_adopted_container(id);
  require(session.operation_history().size() == before + 1,
          "re-enabled logging did not record the next operation");
}

void test_operation_history_records_only_committed_session_facts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout());

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 1,
          "opening a session did not replace history with one event");
  require_operation(history[0],
                    1,
                    OperationKind::session_opened,
                    OperationOutcome::succeeded,
                    L"",
                    wchar_t{},
                    "the session-open event exposed a target or wrong result");

  session.clear_operation_history();
  require(session.operation_history().empty(),
          "clearing history recorded another event");
  static_cast<void>(session.create_container(L"C:\\vault\\work.hc",
                                             64ULL * 1024ULL * 1024ULL));
  history = session.operation_history();
  require(history.size() == 2,
          "successful creation was not split into create and mount facts");
  require_operation(history[0],
                    1,
                    OperationKind::container_created,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    wchar_t{},
                    "creation history retained a path or a drive");
  require_operation(history[1],
                    2,
                    OperationKind::container_mounted,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    L'X',
                    "mount history did not retain the confirmed drive");

  session.unmount_container(1);
  authentication.clear_authentication();
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  history = session.operation_history();
  require(history.size() == 1 && history[0].sequence == 1 &&
              history[0].kind == OperationKind::session_opened,
          "a new authenticated session retained the previous history");
}

void test_operation_history_distinguishes_publish_from_mount_failure() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  session.clear_operation_history();
  runtime.fail_created_mount = true;

  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(session.create_container(L"C:\\vault\\published.hc",
                                                   64ULL * 1024ULL * 1024ULL));
      },
      "the injected post-publication mount failure was accepted");
  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 2,
          "a partial creation did not produce two authoritative facts");
  require_operation(history[0],
                    1,
                    OperationKind::container_created,
                    OperationOutcome::succeeded,
                    L"published.hc",
                    wchar_t{},
                    "the published container was falsely recorded as failed");
  require_operation(history[1],
                    2,
                    OperationKind::container_mounted,
                    OperationOutcome::failed,
                    L"published.hc",
                    wchar_t{},
                    "the failed mount invented a drive letter");
  const std::vector<ContainerStatus> kept = session.container_statuses();
  require(kept.size() == 1 && !kept[0].mounted && kept[0].name == L"published.hc" &&
              remembers(runtime, L"C:\\vault\\published.hc"),
          "the published container was left out of the list and the index");
  runtime.fail_created_mount = false;
  runtime.events.clear();
  session.mount_adopted_container(kept[0].id);
  require(session.container_statuses()[0].mounted,
          "the listed container could not be mounted after the failed create");
}

void test_hidden_container_stays_listed_when_its_mount_fails() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.fail_created_mount = true;
  constexpr std::wstring_view path = L"C:\\vault\\hidden-published.hc";
  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            path, 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
            bytes("decoy-password")));
      },
      "a hidden container mount failure was accepted");
  const std::vector<ContainerStatus> kept = session.container_statuses();
  require(kept.size() == 1 && !kept[0].mounted && !session.mounted() &&
              kept[0].name == L"hidden-published.hc" &&
              runtime.other_existing == std::vector<std::wstring>{std::wstring(path)} &&
              remembers(runtime, path),
          "a published hidden container was dropped when its mount failed");
  runtime.fail_created_mount = false;
  runtime.events.clear();
  session.mount_adopted_container(kept[0].id);
  require(session.container_statuses()[0].mounted &&
              std::find(runtime.events.begin(), runtime.events.end(), "derive:A") ==
                  runtime.events.end(),
          "the listed hidden container was not opened with its creation derivation");
}

void test_operation_history_records_prepublication_and_unmount_failures() {
  {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    const auto session = std::make_unique<CardLoginMountSessionCore>(
        authentication, runtime, layout());
    session->consume_authenticated_login(root_secret(),
                                         bytes("login-password"));
    session->clear_operation_history();
    runtime.fail_create = true;
    require_throws<std::runtime_error>(
        [&] {
          static_cast<void>(session->create_container(
              L"C:\\vault\\failed.hc", 64ULL * 1024ULL * 1024ULL));
        },
        "the injected format failure was accepted");
    const std::vector<OperationEvent> history = session->operation_history();
    require(history.size() == 1,
            "a prepublication failure produced a mount event");
    require_operation(history[0],
                      1,
                      OperationKind::container_created,
                      OperationOutcome::failed,
                      L"failed.hc",
                      wchar_t{},
                      "a failed format was not recorded generically");
  }

  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\work.hc",
                                                    64ULL * 1024ULL * 1024ULL);
  session.clear_operation_history();
  runtime.fail_unmount_a = true;
  require_throws<std::runtime_error>(
      [&] { session.unmount_container(id); },
      "the injected unmount failure was accepted");
  require(session.mounted(), "a failed unmount cleared the live mounted state");
  runtime.fail_unmount_a = false;
  session.unmount_container(id);

  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 2,
          "unmount failure and retry were not both retained");
  require_operation(history[0],
                    1,
                    OperationKind::container_unmounted,
                    OperationOutcome::failed,
                    L"work.hc",
                    L'X',
                    "failed unmount history lost its still-active drive");
  require_operation(history[1],
                    2,
                    OperationKind::container_unmounted,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    L'X',
                    "successful unmount history lost its released drive");
}

void test_operation_history_discards_only_the_oldest_events_at_capacity() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\bounded.hc",
                                                    64ULL * 1024ULL * 1024ULL);
  session.clear_operation_history();
  runtime.fail_unmount_a = true;

  for (std::size_t attempt = 0; attempt < 130; ++attempt) {
    require_throws<std::runtime_error>(
        [&] { session.unmount_container(id); },
        "the injected bounded-history unmount failure was accepted");
  }

  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == usb_vault::application::kOperationHistoryCapacity,
          "operation history exceeded its fixed capacity");
  require(history.front().sequence == 3 &&
              history.front().kind == OperationKind::container_unmounted &&
              history.back().sequence == 130 &&
              history.back().kind == OperationKind::container_unmounted,
          "operation history did not retain the newest events in order");
}

// What the index the fake is holding would show at the next login. Deriving
// the key here the same way the session does is the point: a test that read
// the paths back through the session could not tell a working index from a
// session that simply remembered them in memory.
[[nodiscard]] std::vector<std::wstring> remembered(const FakeRuntime& runtime) {
  if (runtime.container_index.empty()) return {};
  const SecureBuffer key =
      derive_shared_container_index_key_v1(root_secret().bytes());
  return list_container_index_paths_v1(key.bytes(), runtime.container_index);
}

[[nodiscard]] bool remembers(const FakeRuntime& runtime,
                             const std::wstring_view path) {
  for (const std::wstring& entry : remembered(runtime)) {
    if (entry == path) return true;
  }
  return false;
}

// Runs one login's worth of work and closes it again. Cases needing more than
// one login run each of them through here rather than opening them side by
// side: a session core carries the whole operation history by value, so two of
// them in one frame cost more stack than /analyze allows, and no case here
// needs an earlier login to still be open once the next one starts.
template <typename Callable>
void in_one_login(FakeRuntime& runtime, Callable&& work) {
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  std::forward<Callable>(work)(session);
}

void test_container_order_persists_and_failed_writes_preserve_session_order() {
  FakeRuntime runtime;
  runtime.other_existing = {L"D:\\one.hc", L"D:\\two.hc", L"E:\\offline.hc"};
  in_one_login(runtime, [&](CardLoginMountSessionCore& session) {
    const auto one = session.adopt_container(runtime.other_existing[0]);
    const auto two = session.adopt_container(runtime.other_existing[1]);
    const auto offline = session.adopt_container(runtime.other_existing[2]);
    require(session.container_statuses().front().id == offline, "new import did not go first");
    const std::vector<std::uint32_t> order{two, one, offline};
    session.reorder_containers(order);
    require(session.container_statuses().front().id == two && remembered(runtime).front() == L"D:\\two.hc",
            "order was not saved in both session and encrypted index");
    const auto saved = runtime.container_index;
    const std::vector<std::uint32_t> duplicate{one, one, offline};
    const std::vector<std::uint32_t> unknown{one, two, 999};
    const std::vector<std::uint32_t> incomplete{one};
    for (const auto& invalid : {duplicate, unknown, incomplete})
      require_throws<std::invalid_argument>([&] { session.reorder_containers(invalid); }, "invalid order accepted");
    runtime.fail_index_write = true;
    const std::vector<std::uint32_t> changed{one, two, offline};
    require_throws<std::runtime_error>([&] { session.reorder_containers(changed); }, "failed write was ignored");
    require(runtime.container_index == saved && session.container_statuses().front().id == two,
            "failed order write changed state");
    runtime.fail_index_write = false;
    runtime.remember_paths = false;
    const auto reads = runtime.index_reads;
    const auto writes = runtime.index_writes;
    session.reorder_containers(changed);
    require(session.container_statuses().front().id == one && runtime.index_reads == reads && runtime.index_writes == writes,
            "session-only order accessed persistent index");
    runtime.remember_paths = true;
  });
  runtime.other_existing.pop_back();
  in_one_login(runtime, [&](CardLoginMountSessionCore& session) {
    const auto restored = session.container_statuses();
    require(restored.size() == 2 && restored[0].name == L"two.hc" && restored[1].name == L"one.hc",
            "new login did not restore persisted order");
    const std::vector<std::uint32_t> changed{restored[1].id, restored[0].id};
    session.reorder_containers(changed);
    require(remembers(runtime, L"E:\\offline.hc"), "sorting removed an offline container");
  });
}

void test_restored_order_keeps_unmatched_entries_stable_and_preserves_identity() {
  using namespace usb_vault::application;
  FakeRuntime runtime;
  runtime.remember_paths = true;
  runtime.container_a_exists = true;
  runtime.other_existing = {layout().container_b.path, L"D:\\saved.hc"};
  auto file = runtime.read_container_index();
  const auto key = derive_shared_container_index_key_v1(root_secret().bytes());
  ContainerIndexPartition partition{};
  partition.layout = static_cast<std::uint32_t>(usb_vault::volume::PartitionLayout::mbr);
  partition.device_id[0] = 7;
  partition.offset_bytes = 1048576;
  partition.length_bytes = 64ULL * 1024 * 1024;
  partition.logical_sector_bytes = 512;
  require(prepend_container_index_partition(key.bytes(), file, partition,
              PartitionConversionState::paused) == ContainerIndexWriteV1::added,
          "could not prepare paused partition");
  require(prepend_container_index_path_v1(key.bytes(), file, runtime.other_existing[1]) ==
              ContainerIndexWriteV1::added, "could not prepare saved path");
  runtime.container_index = file;
  const auto reads = runtime.index_reads;
  const auto writes = runtime.index_writes;
  in_one_login(runtime, [&](CardLoginMountSessionCore& session) {
    const auto shown = session.container_statuses();
    require(shown.size() == 4, "restoring order lost a container");
    // Layout entries are adopted B then A, followed by the partition and file.
    require(shown[0].id == 4 && shown[1].id == 3 &&
                shown[2].id == 1 && shown[3].id == 2,
            "saved order or stable unmatched order changed");
    require(shown[0].name == L"saved.hc" && !shown[0].partition &&
                shown[1].name == L"磁盘安全盘 1" && shown[1].partition &&
                shown[1].conversion_state == 2 && shown[0].conversion_state == 0 &&
                shown[2].name == L"b.hc" && shown[3].name == L"a.hc",
            "sorting detached identity or conversion state from its container");
    for (const auto& entry : shown)
      require(!entry.mounted && entry.drive_letter == wchar_t{},
              "restoring order changed mount state");
    require(runtime.index_reads == reads + 1 && runtime.index_writes == writes &&
                runtime.container_index == file,
            "restoring order added index I/O or changed ciphertext");
  });
}

void test_every_way_a_container_arrives_writes_it_down() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U'};
  runtime.other_existing.emplace_back(L"D:\\vault\\imported.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  static_cast<void>(session.create_container(L"D:\\vault\\standard.hc",
                                             64ULL * 1024ULL * 1024ULL));
  static_cast<void>(session.create_container_b(
      L"D:\\vault\\hidden.hc", 64ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
  static_cast<void>(session.mount_container(L"D:\\vault\\imported.hc"));

  require(remembers(runtime, L"D:\\vault\\standard.hc"),
          "a standard container was not written into the index");
  require(remembers(runtime, L"D:\\vault\\hidden.hc"),
          "a hidden-volume container was not written into the index");
  require(remembers(runtime, L"D:\\vault\\imported.hc"),
          "an imported container was not written into the index");
  require(remembered(runtime).size() == 3,
          "the index holds something nobody asked it to hold");
}

void test_adopting_a_container_lists_it_without_opening_anything() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  runtime.other_existing.emplace_back(L"D:\\vault\\imported.hc");
  runtime.other_size_bytes = 64ULL * 1024ULL * 1024ULL;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();
  const std::size_t operations_before = session.operation_history().size();

  const std::uint32_t id = session.adopt_container(L"D:\\vault\\imported.hc");

  // Only the existence check runs: no derivation, no drive letter, no mount.
  require_events(runtime, {"exists:final-other"});
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == id &&
              !statuses[0].mounted && statuses[0].drive_letter == wchar_t{},
          "an adopted container did not arrive unmounted");
  require(!session.mounted(), "adopting a container mounted something");
  require(remembers(runtime, L"D:\\vault\\imported.hc"),
          "an adopted container was not written into the index");
  // Nothing is logged: no volume was touched, and the row is the whole event.
  require(session.operation_history().size() == operations_before,
          "adopting a container wrote an operation nobody performed");

  // It knows no more about itself than a remembered path does, so the first
  // mount probes both derivations exactly as an import used to.
  runtime.events.clear();
  session.mount_adopted_container(id);
  require_events(runtime, {"exists:final-other", "derive:A", "allocate",
                           "mount-created:other", "notify:A"});
  require(session.container_statuses()[0].mounted,
          "an adopted container could not be mounted afterwards");
}

void test_adopting_refuses_a_file_that_is_not_there() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S'};
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  require_throws<std::system_error>(
      [&] { static_cast<void>(session.adopt_container(L"D:\\gone.hc")); },
      "a container file that is not there was taken into the list");
  require(session.container_statuses().empty(),
          "a refused adoption left a row behind");
  require(remembered(runtime).empty(),
          "a refused adoption wrote a path down");
}

void test_a_container_already_written_down_is_not_written_again() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  in_one_login(runtime, [](CardLoginMountSessionCore& first) {
    static_cast<void>(first.create_container(L"D:\\vault\\once.hc",
                                             64ULL * 1024ULL * 1024ULL));
  });
  runtime.other_existing.emplace_back(L"D:\\vault\\once.hc");
  runtime.other_size_bytes = 64ULL * 1024ULL * 1024ULL;

  // A second login imports the same file. The index already knows it, so it
  // must be left exactly as it is: a rewrite that changed nothing would still
  // change the file, and the file is what an observer compares.
  const int writes_before = runtime.index_writes;
  CardAuthenticationSession second_login;
  CardLoginMountSessionCore second(second_login, runtime, layout());
  second.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(second.mount_container(L"D:\\vault\\once.hc"));

  require(runtime.index_writes == writes_before,
          "importing a container already in the index rewrote the file");
  require(remembered(runtime).size() == 1,
          "one container ended up in the index twice");
}

void test_forgetting_takes_the_record_and_leaves_the_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t kept = session.create_container(
      L"D:\\vault\\kept.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t dropped = session.create_container(
      L"D:\\vault\\dropped.hc", 64ULL * 1024ULL * 1024ULL);

  require_throws<std::logic_error>(
      [&] { session.forget_container(dropped); },
      "a mounted container was dropped from the session");
  require(remembers(runtime, L"D:\\vault\\dropped.hc"),
          "a refused drop still took the record");

  session.unmount_container(dropped);
  runtime.events.clear();
  session.forget_container(dropped);

  for (const std::string& event : runtime.events) {
    require(!event.starts_with("cleanup"),
            "dropping a record went after the container file");
  }
  require(!remembers(runtime, L"D:\\vault\\dropped.hc"),
          "the dropped container is still in the index");
  require(remembers(runtime, L"D:\\vault\\kept.hc"),
          "dropping one record took another with it");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == kept,
          "the dropped container is still adopted");
  require_throws<std::invalid_argument>(
      [&] { session.forget_container(dropped); },
      "a container that is no longer adopted could be dropped again");
}

void test_an_index_that_cannot_be_reached_costs_only_the_memory() {
  // Remembering is a convenience. A container that exists and is mounted has
  // been created whether or not its path could be written down, so an index
  // that refuses to answer must not turn a finished creation into a failure.
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  runtime.fail_index_read = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::uint32_t id = session.create_container(
      L"D:\\vault\\unrecorded.hc", 64ULL * 1024ULL * 1024ULL);
  require(id != 0 && session.container_statuses().size() == 1 &&
              session.container_statuses()[0].mounted,
          "an unreachable index cost a container that was already created");

  runtime.fail_index_read = false;
  runtime.fail_index_write = true;
  const std::uint32_t second = session.create_container(
      L"D:\\vault\\also-unrecorded.hc", 64ULL * 1024ULL * 1024ULL);
  require(second != 0 && session.container_statuses().size() == 2,
          "an index that cannot be written cost a created container");
  require(remembered(runtime).empty(),
          "a write that was supposed to fail left something behind");

  // And dropping a record is no different: it reports nothing either.
  session.unmount_container(second);
  session.forget_container(second);
  require(session.container_statuses().size() == 1,
          "an unreachable index blocked a container from being dropped");
}

void test_a_login_gets_back_the_containers_whose_files_are_there() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V'};
  in_one_login(runtime, [](CardLoginMountSessionCore& first) {
    static_cast<void>(first.create_container(L"D:\\vault\\here.hc",
                                             64ULL * 1024ULL * 1024ULL));
    static_cast<void>(first.create_container(L"E:\\removable\\away.hc",
                                             64ULL * 1024ULL * 1024ULL));
  });

  // Only one of the two files answers at the next login. The other one is on a
  // drive nobody plugged in.
  std::erase(runtime.other_existing, L"E:\\removable\\away.hc");
  runtime.other_size_bytes = 64ULL * 1024ULL * 1024ULL;

  CardAuthenticationSession second_login;
  CardLoginMountSessionCore second(second_login, runtime, layout());
  second.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::vector<ContainerStatus> statuses = second.container_statuses();
  require(statuses.size() == 1 && statuses[0].name == L"here.hc" &&
              !statuses[0].mounted,
          "the login did not get back exactly the container that is there");

  // The absent one keeps its slot. An unplugged drive is absent in exactly the
  // way a deleted file is, and clearing the record on that evidence would lose
  // it the moment the drive comes back.
  require(remembers(runtime, L"E:\\removable\\away.hc"),
          "a container that was merely unplugged lost its record");
  require(remembered(runtime).size() == 2,
          "the index no longer holds both containers");
}

void test_a_remembered_container_is_opened_by_whichever_derivation_answers() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V'};
  in_one_login(runtime, [](CardLoginMountSessionCore& first) {
    static_cast<void>(first.create_container_b(
        L"D:\\vault\\remembered-b.hc", 64ULL * 1024ULL * 1024ULL,
        16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
  });
  // Publishing already left the file where the fake can find it; from here on
  // only the login derivation opens it, as on a real hidden-volume container.
  runtime.other_size_bytes = 64ULL * 1024ULL * 1024ULL;
  runtime.login_derived_path = L"D:\\vault\\remembered-b.hc";

  CardAuthenticationSession second_login;
  CardLoginMountSessionCore second(second_login, runtime, layout());
  second.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::vector<ContainerStatus> statuses = second.container_statuses();
  require(statuses.size() == 1 && !statuses[0].mounted,
          "the remembered container did not come back unmounted");

  // A slot holds a path and never which password opens it: that would be the
  // tier fact this whole design refuses to write down. So a container read
  // back from the index finds out the way an import does.
  runtime.events.clear();
  second.mount_adopted_container(statuses[0].id);
  require_events(runtime, {"exists:final-other", "derive:A", "allocate",
                           "mount-created:other", "derive:B",
                           "mount-created:other", "notify:A"});
  require(second.container_statuses()[0].mounted,
          "a remembered hidden-volume container could not be mounted");

  // Having found out, it does not go looking again.
  runtime.events.clear();
  second.unmount_container(statuses[0].id);
  second.mount_adopted_container(statuses[0].id);
  require_events(runtime, {"unmount:other", "exists:final-other", "allocate",
                           "mount-created:other", "notify:A"});
}

void test_a_login_that_cannot_read_the_index_still_opens() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  in_one_login(runtime, [](CardLoginMountSessionCore& first) {
    static_cast<void>(first.create_container(L"D:\\vault\\here.hc",
                                             64ULL * 1024ULL * 1024ULL));
  });
  runtime.fail_index_read = true;

  CardAuthenticationSession second_login;
  CardLoginMountSessionCore second(second_login, runtime, layout());
  second.consume_authenticated_login(root_secret(), bytes("login-password"));
  require(second.container_statuses().empty(),
          "an index that cannot be read invented containers");
  require(second.mounted() == false,
          "a login that could not read the index left something mounted");
}

void test_turning_remembering_off_stops_writing_without_erasing() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V'};
  in_one_login(runtime, [](CardLoginMountSessionCore& first) {
    require(first.remembering_container_paths(),
            "the product default is not to remember");
    static_cast<void>(first.create_container(L"D:\\vault\\kept.hc",
                                             64ULL * 1024ULL * 1024ULL));

    first.set_remembering_container_paths(false);
    static_cast<void>(first.create_container(L"D:\\vault\\unrecorded.hc",
                                             64ULL * 1024ULL * 1024ULL));
  });

  // What was written before the switch was flipped stays written. Turning the
  // setting off is an instruction to stop recording, not to forget: flipping
  // it back has to return the same list, which a switch that erased could
  // never do.
  require(remembers(runtime, L"D:\\vault\\kept.hc"),
          "turning remembering off erased what was already recorded");
  require(!remembers(runtime, L"D:\\vault\\unrecorded.hc"),
          "a path was recorded after remembering was turned off");

  in_one_login(runtime, [](CardLoginMountSessionCore& second) {
    require(second.container_statuses().empty(),
            "a login brought the list back with remembering turned off");
    second.set_remembering_container_paths(true);
  });

  CardAuthenticationSession third_login;
  CardLoginMountSessionCore third(third_login, runtime, layout());
  third.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::vector<ContainerStatus> statuses = third.container_statuses();
  require(statuses.size() == 1 && statuses[0].name == L"kept.hc",
          "turning remembering back on did not return the same list");
}

void test_dropping_a_record_works_whether_or_not_remembering_is_on() {
  // Dropping is an explicit instruction about one container, not a policy, so
  // the switch does not gate it. Otherwise a user who turned remembering off
  // could never clear what was written while it was on.
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"D:\\vault\\written.hc", 64ULL * 1024ULL * 1024ULL);
  require(remembers(runtime, L"D:\\vault\\written.hc"),
          "the container was not recorded to begin with");

  session.set_remembering_container_paths(false);
  session.unmount_container(id);
  session.forget_container(id);
  require(!remembers(runtime, L"D:\\vault\\written.hc"),
          "a record could not be cleared once remembering was turned off");
}

// 🔴 A page names a container by the identifier this session handed it and
// never by a drive letter, because a page able to name a letter could name
// any volume on the machine.
void test_the_file_manager_reaches_a_container_by_identifier() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t first = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t second = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);

  runtime.listing.entries.push_back(
      VolumeDirectoryEntry{.name = L"notes.txt"});
  const VolumeDirectoryListing listing =
      session.list_container_directory(first, L"");
  require(listing.entries.size() == 1 &&
              listing.entries[0].name == L"notes.txt",
          "the listing did not come back from the storage layer");
  require(runtime.listed.size() == 1 && runtime.listed[0] == L"X:\\",
          "an empty relative path did not name the first container's root");

  static_cast<void>(
      session.list_container_directory(second, L"reports\\2026"));
  require(runtime.listed.size() == 2 &&
              runtime.listed[1] == L"W:\\reports\\2026",
          "the second container was not reached on its own drive letter");
}

void test_the_file_manager_refuses_a_container_it_cannot_reach() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  require_throws_plain_logic_error(
      [&] { static_cast<void>(session.list_container_directory(1, L"")); },
      "an unauthenticated session served a directory listing");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(session.list_container_directory(id + 100, L""));
      },
      "an identifier no container answers to was accepted");

  // Adopted but not mounted is the case the page will hit most: the container
  // is in the list, and there is nothing behind it to read.
  session.unmount_container(id);
  require_throws_plain_logic_error(
      [&] { static_cast<void>(session.list_container_directory(id, L"")); },
      "an unmounted container was read from anyway");
  require_throws_plain_logic_error(
      [&] { session.delete_container_entry(id, L"", L"notes.txt"); },
      "an unmounted container was written to anyway");

  require(runtime.storage_calls() == 0,
          "a refused request still reached the storage layer");
}

// This is where a page-supplied string becomes a path, so it is where one that
// could leave the container has to stop.
void test_a_path_that_could_leave_the_container_never_reaches_the_disk() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  const std::wstring_view escapes[] = {L"..",
                                       L"..\\..\\Windows",
                                       L"C:\\Windows",
                                       L"\\\\server\\share",
                                       L"\\rooted",
                                       L"reports\\..\\..\\secrets"};
  for (const std::wstring_view relative : escapes) {
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(session.list_container_directory(id, relative));
        },
        "a relative path that leaves the container was resolved");
  }

  // A name is checked as the one component it claims to be, so nothing in it
  // can turn an operation on the entry the user picked into an operation on
  // something else. The colon matters most: `a:b` is a stream on `a`.
  const std::wstring_view names[] = {L"", L"..", L"sub\\file.txt", L"a:b",
                                     L"con"};
  for (const std::wstring_view name : names) {
    require_throws<std::invalid_argument>(
        [&] { session.delete_container_entry(id, L"", name); },
        "a name that is not one plain component was accepted");
  }

  require(runtime.storage_calls() == 0,
          "a refused path still reached the storage layer");
}

void test_deleting_and_creating_name_the_entry_the_page_picked() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  session.delete_container_entry(id, L"reports", L"old.txt");
  require(runtime.deleted.size() == 1 &&
              runtime.deleted[0] == L"X:\\reports\\old.txt",
          "the delete did not name the entry inside the directory shown");

  session.create_container_directory(id, L"", L"archive");
  require(runtime.created_directories.size() == 1 &&
              runtime.created_directories[0] == L"X:\\archive",
          "the new directory was not made where the page was looking");
}

void test_renaming_stays_inside_one_directory() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  session.rename_container_entry(id, L"reports", L"old.txt", L"new.txt");
  require(runtime.moved.size() == 1 &&
              runtime.moved[0].first == L"X:\\reports\\old.txt" &&
              runtime.moved[0].second == L"X:\\reports\\new.txt",
          "a rename did not become a move between two names in one directory");

  require_throws<std::invalid_argument>(
      [&] {
        session.rename_container_entry(id, L"reports", L"old.txt",
                                       L"sub\\new.txt");
      },
      "a new name holding a separator was accepted");
  require(runtime.moved.size() == 1, "the refused rename still moved an entry");
}

// The identifiers alone decide which move this is, because this session never
// puts two containers on one drive letter.
void test_a_move_within_one_container_renames_and_between_two_carries_bytes() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t first = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t second = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);

  session.move_container_entry(first, L"reports", L"q1.txt", first, L"archive");
  require(runtime.moved.size() == 1 && runtime.moved_by_copying.empty() &&
              runtime.moved[0].first == L"X:\\reports\\q1.txt" &&
              runtime.moved[0].second == L"X:\\archive\\q1.txt",
          "a move inside one container did not stay a rename");

  session.move_container_entry(first, L"reports", L"q1.txt", second, L"");
  require(runtime.moved.size() == 1 && runtime.moved_by_copying.size() == 1 &&
              runtime.moved_by_copying[0].first == L"X:\\reports\\q1.txt" &&
              runtime.moved_by_copying[0].second == L"W:\\q1.txt",
          "a move between containers did not become a copy and a delete");

  session.copy_container_entry(second, L"", L"q1.txt", first, L"archive");
  require(runtime.copied.size() == 1 &&
              runtime.copied[0].first == L"W:\\q1.txt" &&
              runtime.copied[0].second == L"X:\\archive\\q1.txt",
          "a copy did not reach across the two containers");

  // The destination is checked as strictly as the source, so a container that
  // is no longer mounted cannot be moved into.
  session.unmount_container(second);
  require_throws_plain_logic_error(
      [&] {
        session.move_container_entry(first, L"", L"q1.txt", second, L"");
      },
      "an unmounted destination container accepted a move");
  require(runtime.moved_by_copying.size() == 1,
          "the refused move still carried an entry");
}

// 🔴 The one entry point that deliberately writes past a volume. It is a
// separate name rather than a flag, so that reaching it is always a decision.
void test_an_export_leaves_the_volume_and_nothing_records_that_it_did() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  const std::size_t events_before = session.operation_history().size();
  const int index_writes_before = runtime.index_writes;

  session.export_container_entry(id, L"reports", L"q1.txt", L"D:\\Backups");
  require(runtime.copied.size() == 1 &&
              runtime.copied[0].first == L"X:\\reports\\q1.txt" &&
              runtime.copied[0].second == L"D:\\Backups\\q1.txt",
          "the export did not carry the entry out to the chosen location");

  // 🔴 Q-04 makes this the product's obligation, not the user's risk: what the
  // user accepted is that the copy may be found, never that this product would
  // write down that they made one.
  require(session.operation_history().size() == events_before,
          "the export was written into the operation history");
  require(runtime.index_writes == index_writes_before,
          "the export touched the container index");

  // The volume root is a location like any other.
  session.export_container_entry(id, L"", L"notes.txt", L"D:\\");
  require(runtime.copied.size() == 2 &&
              runtime.copied[1].first == L"X:\\notes.txt" &&
              runtime.copied[1].second == L"D:\\notes.txt",
          "an export from the volume root did not resolve");
}

void test_an_export_into_a_container_is_refused_as_a_copy() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t first = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  static_cast<void>(session.create_container(L"C:\\vault\\two.hc",
                                             64ULL * 1024ULL * 1024ULL));

  // W is the second container's letter, and X is the first's own. Neither is
  // an export: copying into a container is the file manager's job and carries
  // none of what this call implies.
  for (const std::wstring_view inside : {L"W:\\", L"W:\\reports", L"X:\\",
                                         L"x:\\reports"}) {
    require_throws<std::invalid_argument>(
        [&] {
          session.export_container_entry(first, L"reports", L"q1.txt", inside);
        },
        "an export into a mounted container was accepted");
  }
  require(runtime.copied.empty(),
          "a refused export still reached the storage layer");

  // An unmounted container's letter is nobody's, so the location is ordinary
  // again the moment it is given back.
  session.unmount_container(first);
  const std::uint32_t again =
      session.mount_container(L"C:\\vault\\one.hc");
  session.export_container_entry(again, L"", L"q1.txt", L"X:\\Backups");
  require(runtime.copied.size() == 1 &&
              runtime.copied[0].second == L"X:\\Backups\\q1.txt",
          "a letter no container holds any more was still refused");
}

void test_an_export_location_that_is_not_one_never_reaches_the_disk() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  const std::wstring_view locations[] = {
      L"",                    // nothing at all
      L"Backups",             // relative
      L"D:Backups",           // drive-relative, which is not the D root
      L"\\\\server\\share",   // UNC
      L"\\\\?\\D:\\Backups",  // device namespace
      L"D:\\..\\Windows",     // walks out of what was named
      L"D:\\Back*ups",        // a wildcard is not a directory name
      L"D:\\Backups.",        // Windows would silently strip the dot
  };
  for (const std::wstring_view location : locations) {
    require_throws<std::invalid_argument>(
        [&] {
          session.export_container_entry(id, L"", L"q1.txt", location);
        },
        "an export location that is not one was accepted");
  }

  // The name is still a name, wherever it is going.
  require_throws<std::invalid_argument>(
      [&] {
        session.export_container_entry(id, L"", L"sub\\q1.txt", L"D:\\Backups");
      },
      "an export named a path rather than an entry");

  require(runtime.copied.empty(),
          "a refused export still reached the storage layer");
}

// The migration export: the container's own file, byte for byte. It is what a
// user takes to another machine, where the same card and password open it.
void test_a_container_file_is_migrated_verbatim_and_nothing_else_moves() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  session.unmount_container(id);

  const std::size_t events_before = session.operation_history().size();
  const int index_writes_before = runtime.index_writes;

  session.export_container_file(id, L"D:\\Migration");
  require(runtime.copied.size() == 1 &&
              runtime.copied[0].first == L"C:\\vault\\one.hc" &&
              runtime.copied[0].second == L"D:\\Migration\\one.hc",
          "the migration did not copy the container file under its own name");
  // Nothing is decrypted, nothing is re-encrypted, and nothing else is
  // written: what comes out is the file that went in, which is why it carries
  // no key material and no sign of what is inside it.
  require(runtime.copied.size() == 1 && runtime.moved.empty() &&
              runtime.moved_by_copying.empty() && runtime.deleted.empty(),
          "the migration did more than copy one file");
  require(session.operation_history().size() == events_before,
          "the migration was written into the operation history");
  require(runtime.index_writes == index_writes_before,
          "the migration touched the container index");

  // A container inside a container is still encrypted, so unlike a plaintext
  // export this location is allowed.
  const std::uint32_t other = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2, "the fixture lost a container");
  session.export_container_file(id, L"X:\\Archive");
  require(runtime.copied.size() == 2 &&
              runtime.copied[1].second == L"X:\\Archive\\one.hc",
          "a migration into another container was refused");
  static_cast<void>(other);
}

// Deleting from disk removes the file first and only then the entry; it is
// refused while mounted and when the file is not where the list says.
void test_a_container_is_deleted_from_disk_only_when_safe() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  require_throws_plain_logic_error(
      [&] { session.delete_container_from_disk(id); },
      "a mounted container was deleted from disk");
  session.unmount_container(id);
  const std::size_t events_before = session.operation_history().size();

  // A file that is not there may be on a drive that is not attached.
  std::erase(runtime.other_existing, std::wstring(L"C:\\vault\\one.hc"));
  require_throws<std::runtime_error>(
      [&] { session.delete_container_from_disk(id); },
      "a container whose file is missing was dropped");
  require(runtime.deleted_containers.empty() && session.container_statuses().size() == 1,
          "a refused deletion still changed something");

  runtime.other_existing.emplace_back(L"C:\\vault\\one.hc");
  session.delete_container_from_disk(id);
  require(runtime.deleted_containers.size() == 1 &&
              runtime.deleted_containers[0] == L"C:\\vault\\one.hc",
          "the container file was not deleted");
  require(runtime.protected_cleanups.size() == 1 && runtime.protected_cleanups[0] == L"C:\\vault",
          "the container's folder was not offered for cleanup");
  require(session.container_statuses().empty(), "the deleted container stayed in the list");
  require(session.operation_history().size() == events_before, "the deletion was recorded");
  require_throws<std::invalid_argument>(
      [&] { session.delete_container_from_disk(id); },
      "a deleted container was deleted again");
}

// The migration as the product offers it: an ordinary folder named like the
// vault, the vault file inside under its own name, without Hidden or System.
void test_a_migration_comes_out_as_a_visible_folder() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  require_throws_plain_logic_error(
      [&] { session.export_container_folder(id, L"D:\\Migration", L""); },
      "a mounted container was migrated");
  session.unmount_container(id);
  const std::size_t events_before = session.operation_history().size();
  const int index_writes_before = runtime.index_writes;
  runtime.actual_hidden = true;

  session.export_container_folder(id, L"D:\\Migration", L"工作资料");
  require(runtime.created_directories.size() == 1 &&
              runtime.created_directories[0] == L"D:\\Migration\\工作资料",
          "the migration folder was not named as asked");
  require(runtime.copied.size() == 1 && runtime.copied[0].first == L"C:\\vault\\one.hc" &&
              runtime.copied[0].second == L"D:\\Migration\\工作资料\\one.hc",
          "the vault file did not keep its own name inside the folder");
  require(runtime.visibility_path == L"D:\\Migration\\工作资料\\one.hc" && !runtime.actual_hidden,
          "the migrated vault file kept Hidden and System");
  require(session.operation_history().size() == events_before &&
              runtime.index_writes == index_writes_before,
          "the migration left a trace");

  // An empty name takes the vault's display name.
  session.export_container_folder(id, L"E:\\", L"");
  require(runtime.created_directories.size() == 2 &&
              runtime.created_directories[1] == L"E:\\one.hc",
          "an empty folder name did not fall back to the vault's name");

  // A failed copy takes the new folder away again.
  runtime.fail_copy = true;
  require_throws<std::runtime_error>(
      [&] { session.export_container_folder(id, L"D:\\Out", L"again"); },
      "a failed migration did not report the failure");
  runtime.fail_copy = false;
  require(!runtime.deleted.empty() && runtime.deleted.back() == L"D:\\Out\\again",
          "a failed migration left its folder behind");

  const std::size_t folders = runtime.created_directories.size();
  for (const std::wstring_view name : {L"a\\b", L"..", L"con"}) {
    require_throws<std::invalid_argument>(
        [&] { session.export_container_folder(id, L"D:\\Out", name); },
        "a folder name that is not one was accepted");
  }
  require(runtime.created_directories.size() == folders,
          "a refused folder name still reached the storage layer");
}

// 🔴 The driver is writing to a mounted volume, so a copy taken then is a copy
// of something in flight. Q-04 forbids handing an incomplete copy back as a
// usable migration result.
void test_a_mounted_container_is_not_migrated() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);

  require_throws_plain_logic_error(
      [&] { session.export_container_file(id, L"D:\\Migration"); },
      "a mounted container was copied out from under the driver");
  require(runtime.copied.empty(),
          "the refused migration still reached the storage layer");

  // Giving the volume back makes the file an ordinary one again.
  session.unmount_container(id);
  session.export_container_file(id, L"D:\\Migration");
  require(runtime.copied.size() == 1,
          "an unmounted container was still refused");
}

void test_a_migration_refuses_what_it_cannot_name() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());

  require_throws_plain_logic_error(
      [&] { session.export_container_file(1, L"D:\\Migration"); },
      "an unauthenticated session migrated a container");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  session.unmount_container(id);

  require_throws<std::invalid_argument>(
      [&] { session.export_container_file(id + 100, L"D:\\Migration"); },
      "an identifier no container answers to was accepted");
  for (const std::wstring_view location : {L"", L"Migration", L"D:Migration",
                                           L"\\\\server\\share",
                                           L"D:\\..\\Windows"}) {
    require_throws<std::invalid_argument>(
        [&] { session.export_container_file(id, location); },
        "a migration location that is not one was accepted");
  }
  require(runtime.copied.empty(),
          "a refused migration still reached the storage layer");
}

// 🔴 Q-04's third acceptance item, checked over the whole export surface at
// once. It is the one item that is not the user's risk to carry: what they
// accepted is that a copy may be found, never that this product would write
// down that they made one. So this asks the question of every export entry
// point, of successes and of failures alike, and of every place the session
// can write to.
// A session holding one hidden-volume container, mounted as its login left it.
[[nodiscard]] std::uint32_t maintenance_fixture(
    FakeRuntime& runtime, CardLoginMountSessionCore& session) {
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V'};
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container_b(
      L"D:\\vault\\decoy.hc", 64ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL, bytes("decoy-password"));
  runtime.protected_outer_paths.emplace_back(L"D:\\vault\\decoy.hc");
  runtime.events.clear();
  return id;
}

void test_maintenance_opens_the_outer_volume_under_protection() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);

  const wchar_t drive_letter =
      session.open_decoy_maintenance(id, bytes("decoy-password"));

  // The hidden volume the login put up goes first: one file cannot serve two
  // volumes, and the release is the forced one this flow agreed to.
  require_events(runtime, {"derive:B", "force:other", "allocate",
                           "mount-created:other", "protect", "volume-type",
                           "notify:A"});
  require(drive_letter == L'T', "maintenance took an unexpected drive letter");
  require(runtime.last_mount_label == L"decoy", "maintenance changed the neutral label");
  require(is_b_derivation(runtime.last_mount_password, "decoy-password"),
          "the outer volume was not opened with the password supplied here");
  require(is_b_derivation(runtime.last_protection_password, "login-password"),
          "the hidden volume was not protected with the session derivation");
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].mounted &&
              statuses[0].drive_letter == drive_letter,
          "the container did not come back mounted for maintenance");
}

void test_maintenance_refuses_every_failure_the_same_way() {
  const auto refusal_message = [] {
    FakeRuntime runtime;
    CardAuthenticationSession authentication;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    const std::uint32_t id = maintenance_fixture(runtime, session);
    try {
      static_cast<void>(session.open_decoy_maintenance(id, bytes("wrong")));
    } catch (const DecoyMaintenanceRefused& refused) {
      return std::string(refused.what());
    }
    throw std::runtime_error("a wrong decoy password was accepted");
  }();

  // A hidden header that does not answer -- a container without one, or a
  // session on the other side of the two passwords -- reads identically.
  {
    FakeRuntime runtime;
    CardAuthenticationSession authentication;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    const std::uint32_t id = maintenance_fixture(runtime, session);
    runtime.protection_password_opens = false;
    bool refused = false;
    try {
      static_cast<void>(
          session.open_decoy_maintenance(id, bytes("decoy-password")));
    } catch (const DecoyMaintenanceRefused& rejection) {
      refused = refusal_message == rejection.what();
    }
    require(refused, "a container with no hidden volume read differently");
    require(!session.container_statuses()[0].mounted,
            "a refused maintenance left the container mounted");
  }

  // The driver bringing up the hidden volume instead is what this check is
  // for: refused, and nothing left attached for anyone to write onto.
  {
    FakeRuntime runtime;
    CardAuthenticationSession authentication;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    const std::uint32_t id = maintenance_fixture(runtime, session);
    runtime.protected_volume_type = usb_vault::volume::VolumeType::hidden;
    bool refused = false;
    try {
      static_cast<void>(
          session.open_decoy_maintenance(id, bytes("decoy-password")));
    } catch (const DecoyMaintenanceRefused& rejection) {
      refused = refusal_message == rejection.what();
    }
    require(refused, "the hidden volume coming up read differently");
    require_events(runtime, {"derive:B", "force:other", "allocate",
                             "mount-created:other", "protect", "volume-type",
                             "force:other"});
    require(!session.container_statuses()[0].mounted,
            "the wrong volume was left mounted after a refusal");
  }
}

void test_a_trial_answers_without_leaving_anything_mounted() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  const std::size_t before = session.operation_history().size();

  require(session.verify_decoy_maintenance(id, bytes("decoy-password")),
          "a trial did not recognise the container it can maintain");
  require(runtime.last_mount_label.empty(), "validation probe attempted to change the label");
  require_events(runtime, {"derive:B", "force:other", "allocate",
                           "mount-created:other", "protect", "volume-type",
                           "force:other"});
  require(!session.container_statuses()[0].mounted,
          "a trial left the container mounted");

  // A trial is not an operation performed on the container, so only the
  // release the user was warned about is recorded.
  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == before + 1 &&
              history.back().kind == OperationKind::container_unmounted,
          "a trial recorded more than the release it caused");

  runtime.events.clear();
  require(!session.verify_decoy_maintenance(id, bytes("wrong")),
          "a trial accepted the wrong decoy password");
  require(!session.container_statuses()[0].mounted,
          "a failed trial left the container mounted");
}

void test_maintenance_refuses_before_it_touches_anything() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(
            session.open_decoy_maintenance(1, bytes("decoy-password")));
      },
      "maintenance ran without an authenticated session");

  const std::uint32_t id = maintenance_fixture(runtime, session);
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(
            session.open_decoy_maintenance(id, SecureBuffer()));
      },
      "maintenance accepted an empty password");
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(
            session.open_decoy_maintenance(id + 99, bytes("decoy-password")));
      },
      "maintenance accepted an identifier no container answers to");
  require(runtime.events.empty(),
          "a refused precondition still reached the driver");

  // A container that will not release is not an answer about the password, so
  // it is not folded into the one refusal.
  runtime.fail_manual_force = true;
  bool release_failed = false;
  try {
    static_cast<void>(
        session.open_decoy_maintenance(id, bytes("decoy-password")));
  } catch (const DecoyMaintenanceRefused&) {
    throw std::runtime_error(
        "a container that could not be released reported as a refusal");
  } catch (const std::runtime_error&) {
    release_failed = true;
  }
  require(release_failed,
          "a container that could not be released was still opened");
}

[[nodiscard]] bool listed(const DecoyMaintenanceSurvey& survey,
                          const std::uint32_t id) {
  return std::ranges::find(survey.maintainable, id) !=
         survey.maintainable.end();
}

[[nodiscard]] bool mounted_now(CardLoginMountSessionCore& session,
                               const std::uint32_t id) {
  for (const ContainerStatus& status : session.container_statuses()) {
    if (status.id == id) return status.mounted;
  }
  throw std::runtime_error("the container left the session");
}

void test_creation_yields_the_idle_maintenance_helper_without_unmounting() {
  for (const bool hidden : {false, true}) {
    for (const bool fail : {false, true}) {
      FakeRuntime runtime;
      CardAuthenticationSession authentication;
      CardLoginMountSessionCore session(authentication, runtime, layout());
      const auto maintained = maintenance_fixture(runtime, session);
      const auto drive = session.open_decoy_maintenance(maintained, bytes("decoy-password"));
      const auto identity = runtime.mount_identity(drive);
      static_cast<void>(session.decoy_maintenance_space(maintained));
      require(runtime.maintenance_helper_open, "measurement did not retain its helper");
      runtime.fail_create = fail;
      const auto create = [&] {
        if (hidden) return session.create_container_b(L"D:\\new-hidden.hc",
            64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL, bytes("decoy-password"));
        return session.create_container(L"D:\\new-plain.hc", 64ULL * 1024ULL * 1024ULL);
      };
      if (fail) require_throws<std::runtime_error>([&] { static_cast<void>(create()); },
                                                 "creation failure was lost");
      else require(mounted_now(session, create()), "new container was not mounted");
      require(runtime.end_measure_calls == 1 && !runtime.maintenance_helper_open,
              "creation did not hand off the maintenance helper");
      require(mounted_now(session, maintained) && runtime.mount_identity(drive) == identity &&
                  identity.protected_outer_writable && !session.decoy_maintenance_write_blocked(maintained),
              "creation disturbed the protected maintenance mount");
      static_cast<void>(session.decoy_maintenance_space(maintained));
      require(runtime.maintenance_helper_open && runtime.measure_calls == 2,
              "measurement could not resume after creation");
    }
  }
}

void test_a_survey_lists_only_what_the_password_opens() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V', L'W', L'Z'};
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t standard =
      session.create_container(L"D:\\plain.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t answers = session.create_container_b(
      L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  const std::uint32_t silent = session.create_container_b(
      L"D:\\two.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  runtime.protected_outer_paths.emplace_back(L"D:\\one.hc");
  runtime.events.clear();

  std::vector<std::pair<std::uint32_t, std::uint32_t>> reported;
  const DecoyMaintenanceSurvey survey = session.survey_decoy_maintenance(
      bytes("decoy-password"),
      [&](const std::uint32_t done, const std::uint32_t total) {
        reported.emplace_back(done, total);
      },
      {});

  require(survey.maintainable.size() == 1 && listed(survey, answers),
          "the survey did not list exactly the container it can maintain");
  require(survey.unreachable == 0 && !survey.cancelled,
          "the survey reported trouble it did not have");
  // The standard container is somebody's working disk and has no outer volume
  // to find, so it is neither released nor tried.
  require(mounted_now(session, standard),
          "the survey took away a standard container");
  require(!mounted_now(session, answers) && !mounted_now(session, silent),
          "the survey left a container it tried mounted");
  require(reported.size() == 3 && reported.back().first == 3 &&
              reported.back().second == 3,
          "the survey did not report progress over every container");

  // One password, one derivation, however many containers are walked.
  std::size_t derivations = 0;
  for (const std::string& event : runtime.events) {
    if (event == "derive:B") ++derivations;
  }
  require(derivations == 1, "the survey paid for the KDF more than once");
}

void test_a_survey_counts_containers_it_could_not_release() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V', L'W', L'Z'};
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t stuck = session.create_container_b(
      L"D:\\stuck.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  const std::uint32_t answers = session.create_container_b(
      L"D:\\free.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  runtime.protected_outer_paths.emplace_back(L"D:\\stuck.hc");
  runtime.protected_outer_paths.emplace_back(L"D:\\free.hc");
  // The first container's drive will not come down, so nothing is learned
  // about it and the walk carries on to the next.
  for (const ContainerStatus& status : session.container_statuses()) {
    if (status.id == stuck) runtime.fail_manual_force_drive = status.drive_letter;
  }

  const DecoyMaintenanceSurvey survey =
      session.survey_decoy_maintenance(bytes("decoy-password"), {}, {});

  require(survey.unreachable == 1, "a stuck container was not counted");
  require(!listed(survey, stuck),
          "a container nothing was learned about was offered anyway");
  require(listed(survey, answers),
          "one stuck container stopped the rest of the walk");
}

void test_a_survey_stops_when_asked_and_keeps_nothing_it_did_not_find() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V', L'W', L'Z'};
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(session.create_container_b(
      L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password")));
  static_cast<void>(session.create_container_b(
      L"D:\\two.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password")));

  std::uint32_t tried = 0;
  const DecoyMaintenanceSurvey stopped = session.survey_decoy_maintenance(
      bytes("decoy-password"), {}, [&] { return tried++ == 1; });
  require(stopped.cancelled, "a stopped survey did not say so");

  // Nothing answered, so nothing is held: a password kept here would be one
  // nobody can use.
  require(stopped.maintainable.empty(), "a survey found what does not exist");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.open_surveyed_maintenance(1)); },
      "maintenance opened without a surveyed password");
}

void test_a_surveyed_container_opens_without_asking_again() {
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T', L'U', L'V', L'W', L'Z'};
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container_b(
      L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  runtime.protected_outer_paths.emplace_back(L"D:\\one.hc");
  static_cast<void>(
      session.survey_decoy_maintenance(bytes("decoy-password"), {}, {}));
  runtime.events.clear();

  const wchar_t drive_letter = session.open_surveyed_maintenance(id);
  require_events(runtime, {"allocate", "mount-created:other", "protect",
                           "volume-type", "notify:A"});
  require(mounted_now(session, id) && drive_letter != wchar_t{},
          "the surveyed container did not open");
  require(is_b_derivation(runtime.last_mount_password, "decoy-password"),
          "the surveyed password was not the one that opened the volume");

  const auto added = session.create_container_b(
      L"D:\\new.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  const auto other = session.create_container_b(
      L"D:\\other.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("different-password"));
  runtime.events.clear();
  const auto candidates = session.decoy_maintenance_candidates();
  require(std::ranges::find(candidates, id) != candidates.end() &&
              std::ranges::find(candidates, added) != candidates.end() &&
              std::ranges::find(candidates, other) == candidates.end(),
          "refresh mixed credentials or missed the newly created container");
  require(runtime.events.empty() && mounted_now(session, id) &&
              mounted_now(session, added) && mounted_now(session, other),
          "refresh touched mounted volumes");

  // It goes when the rest of the session's secrets go, and the card leaving
  // is the path that must not be the one exception.
  static_cast<void>(session.force_unmount_active());
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.open_surveyed_maintenance(id)); },
      "a card removal left the surveyed password behind");

  session.release_session_secrets();
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.decoy_maintenance_candidates()); },
      "released session exposed maintenance candidates");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.open_surveyed_maintenance(id)); },
      "a released session still held the surveyed password");
}

void test_maintenance_protection_query_is_scoped_and_never_invents_safety() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  require_throws<std::logic_error>([&] { (void)session.decoy_maintenance_write_blocked(1); }, "unauthenticated query accepted");
  const auto id = maintenance_fixture(runtime, session);
  require_throws<std::logic_error>([&] { (void)session.decoy_maintenance_write_blocked(id); }, "ordinary volume query accepted");
  const auto drive = session.open_decoy_maintenance(id, bytes("decoy-password"));
  require(!session.decoy_maintenance_write_blocked(id), "protected outer volume reported blocked");
  runtime.attached_volume_types[drive] = usb_vault::volume::VolumeType::outer_write_prevented;
  require(session.decoy_maintenance_write_blocked(id), "protection trigger not reported");
  runtime.attached_volume_types[drive] = usb_vault::volume::VolumeType::normal;
  require_throws<std::runtime_error>([&] { (void)session.decoy_maintenance_write_blocked(id); }, "unexpected volume reported safe");
  runtime.attached_volume_types.erase(drive);
  require_throws<std::runtime_error>([&] { (void)session.decoy_maintenance_write_blocked(id); }, "missing volume reported safe");
  require(runtime.measure_calls == 0, "status query triggered elevation");
}

void test_only_a_volume_opened_for_maintenance_is_measured() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  const std::uint32_t standard =
      session.create_container(L"D:\\plain.hc", 64ULL * 1024ULL * 1024ULL);

  // A container mounted the ordinary way is not measured: measuring asks the
  // user for consent, and nothing else in the product spends it.
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.decoy_maintenance_space(standard)); },
      "an ordinary mount was measured");
  require(runtime.measure_calls == 0, "consent was spent on an ordinary mount");

  const wchar_t drive_letter =
      session.open_decoy_maintenance(id, bytes("decoy-password"));
  runtime.listing.entries.push_back(
      VolumeDirectoryEntry{.name = L"decoy.txt"});
  const VolumeDirectoryListing listing =
      session.list_container_directory(id, L"");
  require(listing.entries.size() == 1 &&
              listing.entries[0].name == L"decoy.txt" &&
              runtime.listed.back() == std::wstring(1, drive_letter) + L":\\",
          "the existing file manager did not take over the outer volume");
  const DecoyMaintenanceSpace space = session.decoy_maintenance_space(id);
  require(space.total_bytes == runtime.reported_total &&
              space.free_bytes == runtime.reported_free &&
              space.safe_free_bytes == runtime.reported_safe && space.tail_free_bytes == 0,
          "the space report did not carry what the runtime measured");
  require(runtime.space_drive == drive_letter &&
              runtime.measured_drive == drive_letter,
          "the report measured a volume other than the one opened");

  runtime.fail_measure = true;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(session.decoy_maintenance_space(id)); },
      "a failed refresh was reported as capacity");
  require(runtime.end_measure_calls == 0,
          "one failed refresh ended the measuring run");
  runtime.fail_measure = false;
  static_cast<void>(session.decoy_maintenance_space(id));
  require(runtime.measure_calls == 3,
          "the measuring run could not continue after one failed refresh");
}

void test_measurement_rejects_replaced_mounts() {
  for (const bool during : {false, true}) {
    FakeRuntime runtime;
    CardAuthenticationSession authentication;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    const auto id=maintenance_fixture(runtime, session);
    const auto drive=session.open_decoy_maintenance(id,bytes("decoy-password"));
    static_cast<void>(session.decoy_maintenance_space(id));
    if(during) runtime.change_identity_during_measure=true;
    else ++runtime.identities.at(drive).unique_id;
    require_throws<std::runtime_error>([&]{static_cast<void>(session.decoy_maintenance_space(id));},
                                      "stale mount returned capacity");
    require(runtime.measure_calls==(during?2U:1U) && runtime.end_measure_calls==1,
            "stale binding was used or its helper session remained open");
  }
}

void test_measurement_requires_live_authentication() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication,runtime,layout());
  const auto id=maintenance_fixture(runtime,session);
  static_cast<void>(session.open_decoy_maintenance(id,bytes("decoy-password")));
  authentication.clear_authentication();
  require_throws<std::logic_error>([&]{static_cast<void>(session.decoy_maintenance_space(id));},
                                  "expired authentication reached measurement");
  require(runtime.measure_calls==0,"credentials sent after authentication expired");
}

void test_maintenance_identity_failure_releases_new_mount() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const auto id=maintenance_fixture(runtime, session);
  runtime.fail_identity=true;
  require_throws<DecoyMaintenanceRefused>([&]{static_cast<void>(session.open_decoy_maintenance(id,bytes("decoy-password")));},
                                         "maintenance accepted missing identity");
  require(!mounted_now(session,id) && runtime.measure_calls==0,
          "failed identity left a usable maintenance mount");
}

void test_ending_maintenance_releases_the_volume_and_the_elevation() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  const std::uint32_t ordinary =
      session.create_container(L"D:\\plain.hc", 64ULL * 1024ULL * 1024ULL);
  require_throws<std::logic_error>(
      [&] { session.end_decoy_maintenance(ordinary); },
      "ending maintenance released an ordinary mount");
  require(mounted_now(session, ordinary) && runtime.end_measure_calls == 0,
          "a refused end changed the ordinary mount or measuring run");

  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  runtime.events.clear();

  session.end_decoy_maintenance(id);
  // A deliberate release, not the forced one: ending maintenance is not an
  // emergency and a volume still in use stays the user's to settle.
  require_events(runtime, {"unmount:other"});
  require(runtime.end_measure_calls == 1, "the elevated run was left open");
  require(!mounted_now(session, id),
          "ending maintenance left the container mounted");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.decoy_maintenance_space(id)); },
      "a released container was still treated as open for maintenance");

  // Whatever ends the session ends the run with it.
  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  session.release_session_secrets();
  require(runtime.end_measure_calls == 2,
          "releasing the session left an elevated run behind");
}

void test_every_unmount_path_ends_a_measuring_run() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);

  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  static_cast<void>(session.decoy_maintenance_space(id));
  session.unmount_container(id);
  require(runtime.end_measure_calls == 1,
          "a generic container unmount left the measuring run open");

  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  static_cast<void>(session.decoy_maintenance_space(id));
  session.unmount_active();
  require(runtime.end_measure_calls == 2,
          "a whole-session unmount left the measuring run open");
}

void test_multiple_maintenance_mounts_share_one_helper() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t first = maintenance_fixture(runtime, session);
  runtime.free_drive_letters.push_back(L'W');
  runtime.free_drive_letters.push_back(L'Z');
  const std::uint32_t second = session.create_container_b(
      L"D:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL, bytes("decoy-password"));
  runtime.protected_outer_paths.emplace_back(L"D:\\vault\\second.hc");

  static_cast<void>(
      session.open_decoy_maintenance(first, bytes("decoy-password")));
  static_cast<void>(session.decoy_maintenance_space(first));
  session.import_external_entry(first, L"", L"C:\\source\\first.txt", {});
  const auto closed_imports = runtime.end_import_calls;
  static_cast<void>(
      session.open_surveyed_maintenance(second));

  require(mounted_now(session, first) && mounted_now(session, second),
          "switching maintenance released an outer volume");
  require(runtime.end_measure_calls == 1,
          "switching containers retained the old helper binding");
  require(runtime.end_import_calls == closed_imports,
          "switching maintenance unexpectedly unmounted a volume");
  static_cast<void>(session.decoy_maintenance_space(second));
  session.import_external_entry(second, L"", L"C:\\source\\second.txt", {});
  require(runtime.protected_import_calls == 2,
          "the switched maintenance mount could not import");
  static_cast<void>(session.open_surveyed_maintenance(first));
  static_cast<void>(session.decoy_maintenance_space(first));
  require(mounted_now(session, first) && mounted_now(session, second),
          "returning to an existing maintenance mount released a volume");
  require_throws_plain_logic_error([&] {
    static_cast<void>(session.open_decoy_maintenance(second, bytes("different-password")));
    // Already mounted targets retain their original credential.
    const auto third = session.create_container_b(L"D:\\vault\\third.hc", 64ULL * 1024ULL * 1024ULL,
        16ULL * 1024ULL * 1024ULL, bytes("different-password"));
    static_cast<void>(session.open_decoy_maintenance(third, bytes("different-password")));
  }, "a different credential replaced active maintenance credentials");
  const auto first_drive = session.open_surveyed_maintenance(first);
  const auto second_drive = session.open_surveyed_maintenance(second);
  runtime.attached_volume_types[first_drive] = usb_vault::volume::VolumeType::outer_write_prevented;
  require(session.decoy_maintenance_write_blocked(first) && !session.decoy_maintenance_write_blocked(second),
          "protection status leaked between maintenance volumes");
  require(first_drive != second_drive, "maintenance volumes shared a drive letter");
  session.end_decoy_maintenance(second);
  require(mounted_now(session, first) && !mounted_now(session, second),
          "ending one maintenance volume released another");
  require(session.decoy_maintenance_write_blocked(first),
          "ending another volume cleared the first volume protection");
  session.end_decoy_maintenance(first);
}

void test_a_shell_drop_lands_in_the_directory_the_page_named() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  const wchar_t drive_letter =
      session.open_decoy_maintenance(id, bytes("decoy-password"));
  const std::wstring root(1, drive_letter);
  const std::size_t history_before = session.operation_history().size();

  unsigned int percent=0;
  session.import_external_entry(id, L"", L"C:\\Users\\pictures\\2011.jpg", {}, [&](unsigned int value){percent=value;});
  require(percent==50,"protected import progress was not forwarded");
  session.import_external_entry(id, L"photos/2011",
                                L"D:\\scans\\receipt.pdf", {});
  require(runtime.copied.size() == 2, "a drop did not reach the storage layer");
  require(runtime.copied[0].first == L"C:\\Users\\pictures\\2011.jpg" &&
              runtime.copied[0].second == root + L":\\2011.jpg",
          "a drop on the root did not keep the name the shell handed over");
  require(runtime.copied[1].second == root + L":\\photos\\2011\\receipt.pdf",
          "a drop did not land in the directory the page named");

  // The source is an ordinary OS path and stays one: nothing resolves it
  // against the volume, and nothing about it is written down.
  require(runtime.moved.empty() && runtime.moved_by_copying.empty() &&
              runtime.deleted.empty(),
          "an import moved or removed something");
  require(session.operation_history().size() == history_before,
          "an import was written into the operation history");
}

void test_a_shell_drop_reaches_only_a_volume_open_for_maintenance() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  const std::uint32_t ordinary =
      session.create_container(L"D:\\plain.hc", 64ULL * 1024ULL * 1024ULL);

  // Mounted the ordinary way, so the file manager owns it and a drag onto the
  // window has no business writing into it.
  require_throws_plain_logic_error(
      [&] {
        session.import_external_entry(ordinary, L"", L"C:\\a\\b.txt", {});
      },
      "a drop reached a container mounted the ordinary way");
  require_throws<std::invalid_argument>(
      [&] {
        session.import_external_entry(4242, L"", L"C:\\a\\b.txt", {});
      },
      "a drop reached an identifier no container answers to");

  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  session.end_decoy_maintenance(id);
  require_throws_plain_logic_error(
      [&] { session.import_external_entry(id, L"", L"C:\\a\\b.txt", {}); },
      "a drop reached a maintenance volume that had been released");
  require(runtime.copied.empty(), "a refused drop still copied something");
}

void test_a_shell_drop_cannot_name_anything_outside_the_directory() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));

  // A source with no last component leaves nothing to name the copy after,
  // and a relative directory that could climb out is refused where every
  // other page-supplied path is.
  for (const wchar_t* const source :
       {L"C:\\\\", L"C:\\photos\\\\", L"C:\\photos\\..", L"C:\\photos\\a:b.txt"}) {
    require_throws<std::invalid_argument>(
        [&] { session.import_external_entry(id, L"", source, {}); },
        "a drop named something that is not one entry in this directory");
  }
  for (const wchar_t* const directory :
       {L"..", L"photos/../..", L"C:\\elsewhere", L"\\\\server\\share"}) {
    require_throws<std::invalid_argument>(
        [&] {
          session.import_external_entry(id, directory, L"C:\\a\\b.txt", {});
        },
        "a drop landed outside the volume the session named");
  }
  require(runtime.copied.empty(), "a refused drop still reached the disk");
}

void test_a_shell_drop_carries_its_cancellation_to_the_storage_layer() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));

  bool stop = false;
  const usb_vault::volume::TransferCancellation cancelled = [&stop] {
    return stop;
  };
  session.import_external_entry(id, L"", L"C:\\a\\b.txt", cancelled);
  require(runtime.copied.size() == 1, "a running import did not copy");

  stop = true;
  require_throws<std::system_error>(
      [&] {
        session.import_external_entry(id, L"", L"C:\\a\\c.txt", cancelled);
      },
      "a cancelled import was reported as a copy");
  require(runtime.copied.size() == 1, "a cancelled import still copied");
}

void test_import_rechecks_identity_before_and_after_copy() {
  for(bool during:{false,true}) {
    FakeRuntime runtime;CardAuthenticationSession authentication;
    CardLoginMountSessionCore session(authentication,runtime,layout());
    const auto id=maintenance_fixture(runtime,session);
    const auto drive=session.open_decoy_maintenance(id,bytes("decoy-password"));
    if(during)runtime.change_identity_during_import=true;else ++runtime.identities.at(drive).unique_id;
    require_throws<std::runtime_error>([&]{session.import_external_entry(id,L"",L"C:\\file.bin",{});},"changed upload identity accepted");
    require(runtime.protected_import_calls==(during?1U:0U),"stale upload reached wrong boundary");
    require_throws_plain_logic_error([&]{session.import_external_entry(id,L"",L"C:\\next.bin",{});},"changed mount did not latch uploads");
  }
}

void test_failed_import_latches_until_maintenance_remount() {
  FakeRuntime runtime;CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication,runtime,layout());
  const auto id=maintenance_fixture(runtime,session);
  static_cast<void>(session.open_decoy_maintenance(id,bytes("decoy-password")));
  runtime.fail_copy=true;
  require_throws<std::runtime_error>([&]{session.import_external_entry(id,L"",L"C:\\file.bin",{});},"copy failure lost");
  const auto attempts=runtime.protected_import_calls;
  runtime.fail_copy=false;
  require_throws_plain_logic_error([&]{session.import_external_entry(id,L"",L"C:\\next.bin",{});},"failed maintenance upload restarted");
  require(runtime.protected_import_calls==attempts&&runtime.end_import_calls>0,"failed upload reached helper again");
  session.end_decoy_maintenance(id);static_cast<void>(session.open_decoy_maintenance(id,bytes("decoy-password")));
  session.import_external_entry(id,L"",L"C:\\next.bin",{});
  require(runtime.protected_import_calls==attempts+1,"fresh maintenance did not recover upload");
}

void test_a_shell_drop_needs_an_authenticated_session() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  const std::uint32_t id = maintenance_fixture(runtime, session);
  static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy-password")));
  authentication.clear_authentication();

  require_throws_plain_logic_error(
      [&] { session.import_external_entry(id, L"", L"C:\\a\\b.txt", {}); },
      "a drop was accepted after the card session ended");
  require(runtime.copied.empty(), "a drop copied without a card session");
}

// P5.9: the encrypted export reaches the storage layer with the resolved
// entry and package path, refuses what the plaintext export refuses, and
// leaves nothing behind whether it succeeds, fails or is refused.
void test_selection_export_resolves_vault_groups_and_guards_routes() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.remember_paths = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const auto first = session.create_container(L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const auto second = session.create_container(L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
  session.rename_container(first, L"Documents");
  session.rename_container(second, L"Documents");
  const auto history = session.operation_history().size();
  const auto writes = runtime.index_writes;
  const std::vector<usb_vault::application::ExportSelectionEntry> selection{
      {first, L"", L"same.txt"}, {second, L"sub", L"same.txt"}};
  session.export_container_selection(selection, L"D:\\Out", L"Combined", false, SecureBuffer{}, {}, {});
  require(runtime.selected_sources.size() == 2 &&
      runtime.selected_sources[0].output_name == L"Documents\\same.txt" &&
      runtime.selected_sources[1].output_name == L"Documents (2)\\sub\\same.txt" &&
      runtime.selected_destination == L"D:\\Out\\Combined" && !runtime.selected_encrypted,
      "selection exports must group by display name and disambiguate equal vault names");
  require(session.operation_history().size() == history && runtime.index_writes == writes,
      "selection exports must not record history or indexes");
  const SecureBuffer password = bytes("export password 123");
  session.export_container_selection(selection, L"D:\\Out", L"Combined", true, password, {}, {});
  require(runtime.selected_encrypted, "cipher selection exports must reach the encrypted path");
  require_throws<std::invalid_argument>([&] {
    session.export_container_selection(selection, L"X:\\Out", L"Combined", true, password, {}, {});
  }, "exports must refuse destinations on mounted containers");
  session.unmount_container(second);
  require_throws<std::logic_error>([&] {
    session.export_container_selection(selection, L"D:\\Out", L"Combined", false, SecureBuffer{}, {}, {});
  }, "an unmounted selection must fail before storage");
  require_throws<std::invalid_argument>([&] {
    session.export_container_selection({}, L"D:\\Out", L"Combined", false, SecureBuffer{}, {}, {});
  }, "an empty selection must be refused");
}

void test_an_encrypted_export_leaves_no_trace_and_refuses_containers() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t idle = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
  session.unmount_container(idle);
  const std::size_t events_before = session.operation_history().size();
  const std::size_t runtime_events_before = runtime.events.size();
  const int index_writes_before = runtime.index_writes;
  const auto nothing_was_written = [&](const char* const message) {
    require(session.operation_history().size() == events_before &&
                runtime.events.size() == runtime_events_before &&
                runtime.index_writes == index_writes_before,
            message);
  };
  const SecureBuffer password = bytes("export password 123");

  session.export_container_entry_encrypted(id, L"reports", L"q1.txt", L"D:\\Out",
                                           L"q1 package", password, {}, {});
  require(runtime.encrypted_exports.size() == 1 &&
              runtime.encrypted_exports[0].source == L"X:\\reports\\q1.txt" &&
              runtime.encrypted_exports[0].destination == L"D:\\Out\\q1 package" &&
              runtime.encrypted_exports[0].password_bytes == password.size(),
          "the encrypted export did not reach the storage layer as resolved");
  nothing_was_written("an encrypted export left a local trace");

  runtime.fail_copy = true;
  require_throws<std::runtime_error>(
      [&] {
        session.export_container_entry_encrypted(id, L"", L"q1.txt", L"D:\\Out",
                                                 L"pkg", password, {}, {});
      },
      "a failing encrypted export did not report the failure");
  runtime.fail_copy = false;
  nothing_was_written("a failed encrypted export left a local trace");

  const std::size_t calls = runtime.encrypted_exports.size();
  for (const std::wstring_view inside : {L"X:\\", L"x:\\Out"}) {
    require_throws<std::invalid_argument>(
        [&] {
          session.export_container_entry_encrypted(id, L"", L"q1.txt", inside, L"pkg",
                                                   password, {}, {});
        },
        "an encrypted export into a mounted container was accepted");
  }
  require_throws<std::invalid_argument>(
      [&] {
        session.export_container_entry_encrypted(id, L"", L"q1.txt", L"D:\\Out",
                                                 L"sub\\pkg", password, {}, {});
      },
      "a package name that is a path was accepted");
  require_throws<std::invalid_argument>(
      [&] {
        session.export_container_entry_encrypted(id, L"", L"q1.txt", L"D:\\Out",
                                                 L"pkg", SecureBuffer{}, {}, {});
      },
      "an encrypted export without a password was accepted");
  require_throws_plain_logic_error(
      [&] {
        session.export_container_entry_encrypted(idle, L"", L"q1.txt", L"D:\\Out",
                                                 L"pkg", password, {}, {});
      },
      "an encrypted export from an unmounted container was accepted");
  require(runtime.encrypted_exports.size() == calls,
          "a refused encrypted export still reached the storage layer");
  nothing_was_written("a refused encrypted export left a local trace");
}

// P5.9: an encrypted import names its destination by container and
// directory, refuses an unmounted container, and leaves nothing behind.
void test_an_encrypted_import_reaches_a_mounted_container_without_a_trace() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t idle = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
  session.unmount_container(idle);
  const std::size_t events_before = session.operation_history().size();
  const std::size_t runtime_events_before = runtime.events.size();
  const int index_writes_before = runtime.index_writes;
  const auto nothing_was_written = [&](const char* const message) {
    require(session.operation_history().size() == events_before &&
                runtime.events.size() == runtime_events_before &&
                runtime.index_writes == index_writes_before,
            message);
  };
  const SecureBuffer password = bytes("export password 123");

  require(session.import_package_into_container(id, L"reports", L"D:\\In\\pkg", password, {}, {}) ==
              L"Docs",
          "the import did not return the restored name");
  require(runtime.imports.size() == 1 && runtime.imports[0].source == L"D:\\In\\pkg" &&
              runtime.imports[0].destination == L"X|reports" &&
              runtime.imports[0].password_bytes == password.size(),
          "the import did not reach the storage layer as resolved");
  nothing_was_written("an encrypted import left a local trace");

  runtime.fail_copy = true;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(session.import_package_into_container(id, L"", L"D:\\pkg", password, {}, {})); },
      "a failing import did not report the failure");
  runtime.fail_copy = false;
  nothing_was_written("a failed encrypted import left a local trace");

  const std::size_t calls = runtime.imports.size();
  require_throws_plain_logic_error(
      [&] { static_cast<void>(session.import_package_into_container(idle, L"", L"D:\\pkg", password, {}, {})); },
      "an import into an unmounted container was accepted");
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(session.import_package_into_container(id, L"..", L"D:\\pkg", password, {}, {})); },
      "an import directory leaving the volume was accepted");
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(session.import_package_into_container(id, L"", L"D:\\pkg", SecureBuffer{}, {}, {})); },
      "an import without a password was accepted");
  require(runtime.imports.size() == calls, "a refused import still reached the storage layer");
  nothing_was_written("a refused encrypted import left a local trace");
}

// P5.9: a container created for an import is recorded only once the import
// has succeeded, and one whose import fails is undone without a record.
void test_a_container_created_for_an_import_is_recorded_only_on_success() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::size_t events_before = session.operation_history().size();

  // Any file will do as the package handle: the fake runtime does the write.
  const std::filesystem::path package_path =
      std::filesystem::temp_directory_path() / L"usb-vault-p596-session-package";
  { std::ofstream(package_path, std::ios::binary) << "package"; }
  {
    usb_vault::backup::ImportPackageV1 package(package_path.wstring());
    const SecureBuffer password = bytes("export password 123");

    const int index_writes_before = runtime.index_writes;
    session.defer_operation_records();
    const std::uint32_t kept = session.create_container(L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
    require(session.operation_history().size() == events_before,
            "a container created for an import was recorded before the import");
    require(runtime.index_writes == index_writes_before,
            "a container created for an import reached the index before the import");
    require(session.write_verified_import(kept, L"", package, password, {}, {}) == L"Docs" &&
                runtime.import_writes.size() == 1 && runtime.import_writes[0] == L"X|",
            "the verified import was not written to the new container's root");
    session.settle_operation_records(true);
    require(session.operation_history().size() == events_before + 2,
            "a successful import did not record its new container");
    require(runtime.index_writes > index_writes_before,
            "a successful import did not remember its new container");

    const std::uint32_t other = session.create_container(L"C:\\vault\\three.hc", 64ULL * 1024ULL * 1024ULL);
    const std::size_t events_after = session.operation_history().size();
    const int index_writes_after = runtime.index_writes;
    session.defer_operation_records();
    const std::uint32_t dropped = session.create_container(L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
    // Something unrelated that happens meanwhile is still recorded.
    session.unmount_container(other);
    runtime.fail_copy = true;
    require_throws<std::runtime_error>(
        [&] { static_cast<void>(session.write_verified_import(dropped, L"", package, password, {}, {})); },
        "a failing import write did not report the failure");
    runtime.fail_copy = false;
    session.discard_new_container(dropped);
    session.settle_operation_records(false);
    require(session.operation_history().size() == events_after + 1,
            "a discarded container left a record, or an unrelated record was lost");
    require(runtime.index_writes == index_writes_after,
            "a discarded container reached the index");
    require(runtime.deleted_containers.size() == 1 &&
                runtime.deleted_containers[0] == L"C:\\vault\\two.hc",
            "the discarded container's file was not deleted");
    const auto statuses = session.container_statuses();
    require(statuses.size() == 2 &&
                std::ranges::none_of(statuses, [&](const auto& status) { return status.id == dropped; }),
            "the discarded container stayed in the session");

    session.unmount_container(kept);
    require_throws_plain_logic_error(
        [&] { static_cast<void>(session.write_verified_import(kept, L"", package, password, {}, {})); },
        "a verified import into an unmounted container was accepted");
  }
  std::filesystem::remove(package_path);
}

void test_no_export_leaves_a_single_local_trace() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t mounted = session.create_container(
      L"C:\\vault\\one.hc", 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t idle = session.create_container(
      L"C:\\vault\\two.hc", 64ULL * 1024ULL * 1024ULL);
  session.unmount_container(idle);

  // Everything the session is able to write to, recorded before any export
  // happens. Adding a place the session can write means adding it here.
  const std::size_t events_before = session.operation_history().size();
  const std::size_t runtime_events_before = runtime.events.size();
  const int index_writes_before = runtime.index_writes;
  const int remember_writes_before = runtime.remember_paths_writes;
  const int import_writes_before = runtime.mount_imported_writes;
  const std::vector<std::byte> index_before = runtime.container_index;

  const auto nothing_was_written = [&](const char* const message) {
    require(session.operation_history().size() == events_before &&
                runtime.events.size() == runtime_events_before &&
                runtime.index_writes == index_writes_before &&
                runtime.remember_paths_writes == remember_writes_before &&
                runtime.mount_imported_writes == import_writes_before &&
                runtime.container_index == index_before,
            message);
  };

  session.export_container_entry(mounted, L"reports", L"q1.txt", L"D:\\Out");
  nothing_was_written("a plaintext export left a local trace");

  session.export_container_file(idle, L"D:\\Out");
  nothing_was_written("a migration export left a local trace");

  // 🔴 The case a product usually gets wrong: a failed export is exactly the
  // one an implementation is tempted to record, and recording it would say
  // that an export was attempted on this machine.
  runtime.fail_copy = true;
  require_throws<std::runtime_error>(
      [&] {
        session.export_container_entry(mounted, L"", L"q1.txt", L"D:\\Out");
      },
      "a failing plaintext export did not report the failure");
  nothing_was_written("a failed plaintext export left a local trace");

  require_throws<std::runtime_error>(
      [&] { session.export_container_file(idle, L"D:\\Out"); },
      "a failing migration did not report the failure");
  nothing_was_written("a failed migration left a local trace");
  runtime.fail_copy = false;

  // A refused export must not record the attempt either.
  require_throws<std::invalid_argument>(
      [&] {
        session.export_container_entry(mounted, L"", L"q1.txt", L"Backups");
      },
      "a refused export was accepted");
  nothing_was_written("a refused export left a local trace");

  // And the ordinary operations still do record themselves, so the checks
  // above are about exports rather than about a session that records nothing.
  session.unmount_container(mounted);
  require(session.operation_history().size() == events_before + 1,
          "an unmount stopped being recorded, so the export checks prove "
          "nothing");
}

void test_display_alias_persistence_and_export_identity() {
  using namespace usb_vault::application;
  CardAuthenticationSession auth;
  FakeRuntime runtime;
  runtime.remember_paths = true;
  CardLoginMountSessionCore session(auth, runtime, layout());
  require_throws<std::logic_error>([&] { session.rename_container(1, L"资料"); }, "unauthenticated rename accepted");
  session.consume_authenticated_login(root_secret(), bytes("login"));
  const auto id = session.create_container(L"C:\\vault\\original.hc", 64ULL*1024*1024);
  require(runtime.last_mount_label == L"original", "new file mount omitted its label");
  runtime.events.clear();
  session.rename_container(id, L"工作:资料 \U0001F512");
  require(runtime.events.empty(), "renaming performed a volume operation");
  auto rows = session.container_statuses();
  require(rows.size()==1 && rows[0].mounted && rows[0].name==L"工作:资料 \U0001F512", "mounted rename lost volume or alias");
  const auto saved = runtime.container_index;
  const auto writes = runtime.index_writes;
  session.rename_container(id, L"工作:资料 \U0001F512");
  require(runtime.index_writes==writes && runtime.container_index==saved, "same alias rewrote index");
  runtime.fail_index_write = true;
  require_throws<std::runtime_error>([&] { session.rename_container(id, L"未保存"); }, "failed rename reported success");
  require(session.container_statuses()[0].name==rows[0].name && runtime.container_index==saved,
          "failed rename changed name or index");
  runtime.fail_index_write = false;
  runtime.remember_paths = false;
  require_throws<std::logic_error>([&] { session.rename_container(id, L"关闭"); }, "disabled persistence accepted rename");
  runtime.remember_paths = true;
  for (const auto& invalid : {std::wstring(L"  "), std::wstring(L"a/b"), std::wstring(L"a\\b"),
                             std::wstring(L"a\0b",3), std::wstring(101,L'x')})
    require_throws<std::invalid_argument>([&] { session.rename_container(id, invalid); }, "invalid alias accepted");
  require_throws<std::invalid_argument>([&] { session.rename_container(99999, L"missing"); }, "unknown rename accepted");
  require(runtime.container_index==saved, "rejected rename mutated index");
  session.unmount_container(id);
  require(session.operation_history().back().container_name==rows[0].name, "operation did not use display name");
  session.export_container_file(id, L"D:\\Out");
  require(runtime.copied.back()==std::pair<std::wstring,std::wstring>{L"C:\\vault\\original.hc",L"D:\\Out\\original.hc"},
          "alias changed export filename or source");
  CardAuthenticationSession next_auth;
  CardLoginMountSessionCore next(next_auth, runtime, layout());
  next.consume_authenticated_login(root_secret(), bytes("other-login"));
  auto restored = next.container_statuses();
  require(restored.size()==1 && restored[0].name==rows[0].name, "alias did not survive another valid login");
  require(next.container_operation_targets()[0].name==rows[0].name, "batch target lost alias");
  next.mount_adopted_container(restored[0].id);
  require(runtime.last_mount_label == L"工作_资料 \U0001F512", "remount did not use sanitized saved alias");
  next.unmount_container(restored[0].id);
  next.rename_container(restored[0].id, {});
  require(next.container_statuses()[0].name==L"original.hc", "empty alias did not restore default");
  next_auth.clear_authentication();
  require_throws<std::logic_error>([&] { next.rename_container(restored[0].id,L"revoked"); }, "revoked rename accepted");
}

void test_protected_creation_shows_only_the_display_name() {
  const std::wstring folder = L"C:\\vault\\0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
  const std::wstring path = folder + L"\\{73DC3809-D909-431A-9807-1F8CDAD47A1F}";
  const auto before = [](const FakeRuntime& runtime, const char* first, const char* second) {
    const auto a = std::ranges::find(runtime.events, first);
    return a != runtime.events.end() && a < std::ranges::find(runtime.events, second);
  };
  for (const bool pair : {false, true}) for (const bool remember : {true, false}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.remember_paths = remember;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    const int writes = runtime.index_writes;
    const auto id = pair
        ? session.create_container_b(path, 64ULL*1024*1024, 16ULL*1024*1024, bytes("decoy-password"), L"TEST")
        : session.create_container(path, 64ULL*1024*1024, L"TEST");
    require(runtime.protected_directories == std::vector<std::wstring>{folder}, "protected folder was not created once");
    require(before(runtime, "directory:create", "file-attributes") && before(runtime, "file-attributes", "publish:other"),
            "folder, attributes and publication ran out of order");
    require(runtime.visibility_path == path + L".preparing", "protected file was not hidden regardless of the setting");
    require(runtime.last_mount_label == L"TEST", "volume label exposed the private file name");
    const auto rows = session.container_statuses();
    require(rows.size() == 1 && rows[0].id == id && rows[0].mounted && rows[0].name == L"TEST", "list exposed the private file name");
    int named = 0;
    for (const auto& record : session.operation_history()) {
      if (record.container_name.empty()) continue;  // Session events name no container.
      require(record.container_name == L"TEST", "operation record exposed the private file name");
      ++named;
    }
    require(named == 2, "creation and mount were not both recorded");
    require(runtime.index_writes == writes + (remember ? 1 : 0), "path and alias were not written in exactly one index save");
    CardAuthenticationSession next_auth;
    CardLoginMountSessionCore next(next_auth, runtime, layout());
    next.consume_authenticated_login(root_secret(), bytes("other-login"));
    const auto restored = next.container_statuses();
    if (remember)
      require(restored.size() == 1 && restored[0].name == L"TEST", "remembered protected container lost its display name");
    else
      require(restored.empty(), "disabled persistence still remembered the protected container");
  }

  // Before publication nothing stays behind; a folder that cannot be removed
  // never replaces the error being reported.
  for (const int failure : {0, 1, 2, 3}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    runtime.fail_create = failure == 0 || failure == 3;
    runtime.fail_publish = failure == 1;
    runtime.hiding_fails = failure == 2;
    runtime.remove_directory_fails = failure == 3;
    try {
      static_cast<void>(session.create_container(path, 64ULL*1024*1024, L"TEST"));
      require(false, "failed protected creation reported success");
    } catch (const std::runtime_error& error) {
      const std::string expected = failure == 1 ? "publish failed" : failure == 2 ? "attribute update failed" : "create failed";
      require(error.what() == expected, "folder cleanup replaced the creation error");
    }
    require(std::ranges::count(runtime.events, "directory:remove") == 1, "unpublished folder was not removed exactly once");
    require(runtime.protected_directories.size() == (failure == 3 ? 1u : 0u), "unpublished folder outlived its creation");
    require(runtime.other_existing.empty() && session.container_statuses().empty(), "failed protected creation was listed");
    require(session.operation_history().back().container_name == L"TEST", "failure record exposed the private file name");
  }

  {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    runtime.protect_directory_fails = true;
    require_throws<std::runtime_error>([&] { session.create_container(path, 64ULL*1024*1024, L"TEST"); },
                                       "unprotected folder was used");
    require(std::ranges::find(runtime.events, "create:other") == runtime.events.end() &&
                std::ranges::find(runtime.events, "directory:remove") == runtime.events.end(),
            "a folder this creation did not make was touched");
    runtime.protect_directory_fails = false;
    runtime.events.clear();
    runtime.fail_created_mount = true;
    require_throws<std::runtime_error>([&] { session.create_container(path, 64ULL*1024*1024, L"TEST"); },
                                       "failed mount reported success");
    require(runtime.protected_directories.size() == 1 && runtime.other_existing.size() == 1 &&
                std::ranges::find(runtime.events, "directory:remove") == runtime.events.end(),
            "a published container lost its folder");
    require(session.container_statuses().size() == 1 && session.container_statuses()[0].name == L"TEST",
            "published container was not kept under its display name");
  }

  {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    establish_empty_authenticated_session(authentication, runtime, session);
    for (const auto& name : {std::wstring(L"a/b"), std::wstring(L"  "), std::wstring(101, L'x')})
      require_throws<std::invalid_argument>([&] { session.create_container(path, 64ULL*1024*1024, name); },
                                            "invalid display name accepted");
    require_throws<std::invalid_argument>([&] { session.create_container(L"C:\\{73DC3809-D909-431A-9807-1F8CDAD47A1F}", 64ULL*1024*1024, L"TEST"); },
                                          "protected container accepted a drive root as its folder");
    require(std::ranges::find(runtime.events, "directory:create") == runtime.events.end(), "rejected input created a folder");
  }
}

void test_alias_save_repairs_missing_file_records_and_restores_imports() {
  using namespace usb_vault::application;
  CardAuthenticationSession auth;
  FakeRuntime runtime;
  runtime.remember_paths=false;
  runtime.other_existing.emplace_back(L"D:\\offline.hc");
  CardLoginMountSessionCore session(auth,runtime,layout());
  session.consume_authenticated_login(root_secret(),bytes("login"));
  const auto id=session.adopt_container(L"D:\\offline.hc");
  runtime.remember_paths=true;
  session.rename_container(id,L"移动资料");
  require(remembers(runtime,L"D:\\offline.hc"), "rename did not persist a previously unremembered file");
  const auto saved=runtime.container_index;
  runtime.other_existing.clear();
  CardAuthenticationSession next_auth;
  CardLoginMountSessionCore next(next_auth,runtime,layout());
  next.consume_authenticated_login(root_secret(),bytes("other-login"));
  require(next.container_statuses().empty() && runtime.container_index==saved, "absent file lost record");
  runtime.other_existing.emplace_back(L"D:\\offline.hc");
  const auto restored=next.adopt_container(L"D:\\offline.hc");
  require(next.container_statuses()[0].name==L"移动资料" && runtime.container_index==saved,
          "reimport lost alias or rewrote index");
  next.set_remembering_container_paths(false);
  require_throws<std::logic_error>([&]{next.rename_container(restored,L"禁止");}, "disabled persistence saved alias");
  require(runtime.container_index==saved, "disabling persistence deleted aliases");
  next.set_remembering_container_paths(true);
  next.rename_container(restored,L"恢复保存");
  require(next.container_statuses()[0].name==L"恢复保存", "reenabled persistence cannot save alias");
  runtime.other_size_bytes=64ULL*1024*1024;
  require(next.mount_container(L"D:\\offline.hc")==restored &&
          next.operation_history().back().container_name==L"恢复保存", "path-based mount lost display alias");
  next.unmount_container(restored);
}

void test_partition_alias_survives_conversion_and_restart() {
  using namespace usb_vault::application;
  CardAuthenticationSession auth;
  FakeRuntime runtime;
  runtime.remember_paths=true; runtime.partition_password_64=true;
  runtime.creation_cancelled=[]{return false;}; runtime.creation_revoked=[]{return false;};
  CardLoginMountSessionCore session(auth,runtime,layout());
  session.consume_authenticated_login(root_secret(),bytes("login"));
  usb_vault::volume::PartitionCandidate target;
  target.device_id[0]=7; target.layout=usb_vault::volume::PartitionLayout::mbr;
  target.logical_sector_bytes=512; target.offset_bytes=1048576; target.length_bytes=64ULL*1024*1024;
  runtime.during_conversion=[&] {
    require_throws<std::logic_error>([&]{session.rename_container(session.container_statuses()[0].id,L"进行中");},
                                     "active conversion accepted rename");
  };
  require(session.begin_partition_conversion(target,[](const auto&){return true;}), "conversion did not pause");
  runtime.during_conversion={};
  session.rename_container(session.container_statuses()[0].id,L"备份盘");
  CardAuthenticationSession next_auth;
  CardLoginMountSessionCore next(next_auth,runtime,layout());
  next.consume_authenticated_login(root_secret(),bytes("other-login"));
  const auto id=next.container_statuses()[0].id;
  require(next.container_statuses()[0].name==L"备份盘", "paused partition lost alias on login");
  runtime.conversion_pause=false;
  require(!next.resume_partition_conversion(id), "resume did not finish");
  require(next.container_statuses()[0].name==L"备份盘" && next.container_statuses()[0].mounted,
          "conversion completion overwrote alias");
  next.rename_container(id,{});
  require(next.container_statuses()[0].name==L"磁盘安全盘 1", "partition default number lost");
}

void test_partition_conversion_lifecycle() {
  using namespace usb_vault::application;
  using usb_vault::volume::PartitionCandidate;
  for(int scenario=0;scenario<7;++scenario) {
    CardAuthenticationSession auth; FakeRuntime runtime;
    runtime.partition_password_64=true; runtime.remember_paths=true;
    bool revoked=false;
    runtime.creation_cancelled=[]{return false;}; runtime.creation_revoked=[&]{return revoked;};
    CardLoginMountSessionCore session(auth,runtime,layout());
    session.consume_authenticated_login(root_secret(),bytes("login"));
    PartitionCandidate target; target.device_id[0]=7; target.layout=usb_vault::volume::PartitionLayout::mbr;
    target.logical_sector_bytes=512; target.offset_bytes=1048576; target.length_bytes=64ULL*1024*1024;
    auto key=derive_shared_container_index_key_v1(root_secret().bytes());
    runtime.fail_index_write=scenario==3;
    runtime.during_conversion=[&]{
      const auto records=list_container_index_partition_entries(key.bytes(),runtime.container_index);
      require(records.size()==1 && records[0].conversion==PartitionConversionState::uncertain,"disk write preceded durable uncertain state");
      if(scenario==2)revoked=true;
      if(scenario==4)runtime.fail_index_write=true;
    };
    runtime.conversion_fail=scenario==1;
    if(scenario==5)runtime.partition_password_64=false;
    bool failed=false;
    try {require(session.begin_partition_conversion(target,[&](const auto& frozen){return frozen==target && scenario!=6;}),"conversion did not pause");}
    catch(...){failed=true;}
    require(failed==(scenario!=0),"conversion outcome mismatch");
    const auto rows=session.container_statuses();
    const auto records=list_container_index_partition_entries(key.bytes(),runtime.container_index);
    if(scenario>=3 && scenario!=4) {require(rows.empty() && records.empty(),"unconfirmed operation left a record");continue;}
    require(rows.size()==1 && !rows[0].mounted && rows[0].conversion_state==(scenario==0 ? 2u : 1u),"unsafe conversion state");
    // A converted partition needs a position of its own for the same reason an
    // erase-created one does: a record with no order sorts by the slot it landed
    // in, and that slot is chosen at random on purpose.
    require(records.size()==1 && records[0].order.has_value() && records[0].sequence!=0,
            "a converted partition was recorded without a position or a number");
    require_throws<std::logic_error>([&]{session.mount_adopted_container(rows[0].id);},"incomplete conversion mounted");
    if(scenario!=0) {
      require_throws<std::logic_error>([&]{session.resume_partition_conversion(rows[0].id);},"uncertain conversion resumed");
      if (scenario == 4) {
        require_throws<std::runtime_error>([&]{session.forget_container(rows[0].id);},"failed index write accepted removal");
        require(session.container_statuses().size()==1 &&
                    list_container_index_partition_entries(key.bytes(),runtime.container_index).size()==1,
                "failed removal lost the conversion record");
      }
      runtime.fail_index_write=false;
      runtime.events.clear();
      session.forget_container(rows[0].id);
      require(session.container_statuses().empty() &&
                  list_container_index_partition_entries(key.bytes(),runtime.container_index).empty(),
              "incomplete conversion record survived removal");
      require(std::ranges::none_of(runtime.events, [](const auto& event) {
                return event.starts_with("cleanup:") || event.starts_with("create:") || event.starts_with("mount:");
              }), "forgetting conversion touched the container");
      continue;
    }
    auth.clear_authentication();
    require_throws<std::logic_error>([&]{session.resume_partition_conversion(rows[0].id);},"resume bypassed authentication");
    CardAuthenticationSession next_auth;
    CardLoginMountSessionCore next(next_auth,runtime,layout());
    next.consume_authenticated_login(root_secret(),bytes("other-valid-login"));
    const auto restored=next.container_statuses(); require(restored.size()==1 && restored[0].conversion_state==2,"paused state lost on reopen");
    runtime.wrong_conversion_target=true;
    require_throws<std::logic_error>([&]{next.resume_partition_conversion(restored[0].id);},"changed resume identity accepted");
    runtime.wrong_conversion_target=false; runtime.conversion_pause=false;
    require(!next.resume_partition_conversion(restored[0].id),"completed conversion reported paused");
    require(next.container_statuses()[0].mounted && next.container_statuses()[0].conversion_state==0,"completed conversion not released for mounting");
    require(list_container_index_partition_entries(key.bytes(),runtime.container_index)[0].conversion==PartitionConversionState::normal,"completed record not persisted");
    next.unmount_container(restored[0].id);
  }
}

void test_partition_creation_persistence_and_mounting() {
  for (int scenario = 0; scenario != 5; ++scenario) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.partition_password_64 = true;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    runtime.creation_cancelled = [] { return false; };
    runtime.remember_paths = true;
    runtime.fail_index_write = scenario == 1;
    runtime.partition_mount_fail = scenario == 2;
    runtime.partition_fail = scenario == 3;
    unsigned final_stage = 0; wchar_t final_drive{};
    runtime.creation_feedback = [&](unsigned stage, unsigned, wchar_t drive) { final_stage = stage; final_drive = drive; };
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = 7;
    target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024;
    target.logical_sector_bytes = 512;
    bool failed = false;
    std::uint32_t id{};
    try { id = session.create_partition(target, [&](const auto&) { return scenario != 4; }); }
    catch (...) { failed = true; }
    require(failed == (scenario != 0), "partition creation outcome wrong");
    require(runtime.partition_written == (scenario == 0 || scenario == 2), "failed persistence allowed format");
    auto key = usb_vault::application::derive_shared_container_index_key_v1(root_secret().bytes());
    const auto records = usb_vault::application::list_container_index_partitions(key.bytes(), runtime.container_index);
    require(records.size() == ((scenario == 0 || scenario == 2 || scenario == 3) ? 1U : 0U),
            "partition identity was lost or recorded without consent");
    if (scenario != 0) continue;
    require(session.container_statuses().back().mounted, "new partition was not mounted");
    require(runtime.last_mount_label == session.container_statuses().back().name,
            "new partition mount omitted its label");
    require(final_stage == 5 && final_drive == session.container_statuses().back().drive_letter, "partition completion omitted mounted drive");
    require_throws<std::logic_error>([&] { session.export_container_file(id, L"D:\\exports"); },
        "partition ciphertext exported as a file");
    session.unmount_container(id);
    require_throws<usb_vault::application::DecoyMaintenanceRefused>([&] {
      static_cast<void>(session.open_decoy_maintenance(id, bytes("decoy")));
    }, "partition entered hidden maintenance");
    CardAuthenticationSession next_auth;
    CardLoginMountSessionCore next(next_auth, runtime, layout());
    next.consume_authenticated_login(root_secret(), bytes("other-login-password"));
    const auto restored = next.container_statuses();
    require(restored.size() == 1 && !restored[0].mounted, "partition was not restored after login");
    next.mount_adopted_container(restored[0].id);
    require(next.container_statuses()[0].mounted, "restored partition could not mount");
    next.unmount_container(restored[0].id);
    next.forget_container(restored[0].id);
    require(usb_vault::application::list_container_index_partitions(key.bytes(), runtime.container_index).empty(),
            "forget did not remove partition record");
  }
}

void test_a_disk_container_keeps_its_name_through_a_drag_and_a_restart() {
  // The name used to be derived from where the container sat, so rearranging the
  // list renamed things and every login renumbered them. It is recorded with the
  // container now: the list may be reordered freely and the names must not move.
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.partition_password_64 = true;
  runtime.remember_paths = true;
  runtime.free_drive_letters = {L'X', L'W', L'V', L'U', L'T'};
  runtime.creation_cancelled = [] { return false; };
  const auto create = [&](CardLoginMountSessionCore& session, std::uint8_t device) {
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = device;
    target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024;
    target.logical_sector_bytes = 512;
    return session.create_partition(target, [](const auto&) { return true; });
  };
  const auto disk_names = [](CardLoginMountSessionCore& session) {
    std::vector<std::wstring> names;
    for (const auto& status : session.container_statuses())
      if (status.partition) names.push_back(status.name);
    return names;
  };

  std::vector<std::wstring> after_drag;
  {
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    const auto first = create(session, 7);
    const auto second = create(session, 8);
    const auto third = create(session, 9);
    require(disk_names(session) == std::vector<std::wstring>{L"磁盘安全盘 3", L"磁盘安全盘 2", L"磁盘安全盘 1"},
            "creation did not number disk containers in the order they were made");

    // Oldest to the top, newest into the middle: a position no numbering implies.
    std::vector<std::uint32_t> dragged{first, third, second};
    for (const auto& status : session.container_statuses())
      if (!status.partition) dragged.push_back(status.id);
    session.reorder_containers(dragged);
    after_drag = disk_names(session);
    require(after_drag == std::vector<std::wstring>{L"磁盘安全盘 1", L"磁盘安全盘 3", L"磁盘安全盘 2"},
            "dragging the list renamed the disk containers");
  }

  // A fresh session over the same index is what a restart looks like: both the
  // arrangement and the names have to come back exactly.
  CardAuthenticationSession next_auth;
  CardLoginMountSessionCore next(next_auth, runtime, layout());
  next.consume_authenticated_login(root_secret(), bytes("login-password"));
  require(disk_names(next) == after_drag,
          "a restart changed the disk container names or their order");

  // The new container reuses the smallest gap without renaming existing rows.
  for (const auto& status : next.container_statuses())
    if (status.partition && status.name == L"磁盘安全盘 2") next.forget_container(status.id);
  static_cast<void>(create(next, 10));
  const auto names = disk_names(next);
  require(std::ranges::find(names, std::wstring(L"磁盘安全盘 2")) != names.end() &&
              std::ranges::find(names, std::wstring(L"磁盘安全盘 4")) == names.end(),
          "the smallest unused disk container number was not reused");
}

void test_a_dragged_order_records_disk_containers_too() {
  // A disk container had no position of its own in the index: ordering wrote
  // only the file paths, so its place came back from the slot it happened to
  // land in, and that slot is chosen at random on purpose. What the user drags
  // has to reach the index for both kinds.
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.partition_password_64 = true;
  runtime.remember_paths = true;
  runtime.free_drive_letters = {L'X', L'W', L'V', L'U', L'T'};
  runtime.creation_cancelled = [] { return false; };
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const auto identity_of = [](std::uint8_t device) {
    usb_vault::volume::PartitionIdentity identity{};
    identity.device_id[0] = device;
    identity.layout = static_cast<std::uint32_t>(usb_vault::volume::PartitionLayout::mbr);
    identity.logical_sector_bytes = 512;
    identity.offset_bytes = 1048576;
    identity.length_bytes = 64ULL * 1024 * 1024;
    return identity;
  };
  const auto create = [&](std::uint8_t device) {
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = device;
    target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024;
    target.logical_sector_bytes = 512;
    return session.create_partition(target, [](const auto&) { return true; });
  };

  const auto first = create(7);
  const auto second = create(8);
  const auto third = create(9);
  // Newest first, each with the next number.
  const auto shown = session.container_statuses();
  std::vector<std::wstring> names;
  for (const auto& status : shown)
    if (status.partition) names.push_back(status.name);
  require(names.size() == 3 && names[0] == L"磁盘安全盘 3" &&
              names[1] == L"磁盘安全盘 2" && names[2] == L"磁盘安全盘 1",
          "a newly created disk container did not go first with the next number");

  // Drag them into an order no slot layout would produce, oldest in the middle.
  std::vector<std::uint32_t> dragged;
  for (const auto& status : shown) if (!status.partition) dragged.push_back(status.id);
  dragged.insert(dragged.begin(), {second, first, third});
  session.reorder_containers(dragged);

  const auto key = usb_vault::application::derive_shared_container_index_key_v1(root_secret().bytes());
  const auto refs = usb_vault::application::list_container_index_refs_v1(
      key.bytes(), runtime.container_index);
  std::vector<usb_vault::volume::PartitionIdentity> recorded;
  for (const auto& ref : refs) if (ref.partition) recorded.push_back(*ref.partition);
  require(recorded.size() == 3 && recorded[0] == identity_of(8) &&
              recorded[1] == identity_of(7) && recorded[2] == identity_of(9),
          "the dragged order did not reach the index for disk containers");
  require(usb_vault::application::remove_container_index_partition(key.bytes(), runtime.container_index, identity_of(8)),
          "missing-index fixture could not remove the record");
  session.reorder_containers(dragged);
  require(session.container_statuses().front().id == second,
          "an unrecorded partition lost its requested session position");
  const auto remaining = usb_vault::application::list_container_index_partitions(key.bytes(), runtime.container_index);
  require(remaining == std::vector<usb_vault::volume::PartitionIdentity>{identity_of(7), identity_of(9)},
          "sorting failed to skip a missing partition or restored it unexpectedly");
}

void test_disk_containers_are_numbered_among_themselves() {
  // A disk container has no file name, so it is numbered. The number used to
  // come from the id every container shares, so a file container adopted in
  // between consumed one and the disk containers came back with gaps.
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.partition_password_64 = true;
  runtime.remember_paths = true;
  // Four containers are created and each keeps its mount, so the fake needs
  // more letters than its default three.
  runtime.free_drive_letters = {L'X', L'W', L'V', L'U', L'T'};
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.creation_cancelled = [] { return false; };

  const auto make_partition = [&](const std::uint8_t device) {
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = device;
    target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024;
    target.logical_sector_bytes = 512;
    return session.create_partition(target, [](const auto&) { return true; });
  };

  static_cast<void>(make_partition(7));
  // A file container in between takes a container id but must not take a disk
  // container's number.
  static_cast<void>(session.create_container(L"C:\\vault\\between.hc", 64ULL * 1024 * 1024));
  static_cast<void>(make_partition(8));
  static_cast<void>(make_partition(9));

  std::vector<std::wstring> disk_names;
  for (const auto& status : session.container_statuses()) {
    if (status.name.starts_with(L"磁盘安全盘 ")) disk_names.push_back(status.name);
  }
  std::ranges::sort(disk_names);
  require(disk_names.size() == 3 && disk_names[0] == L"磁盘安全盘 1" &&
              disk_names[1] == L"磁盘安全盘 2" && disk_names[2] == L"磁盘安全盘 3",
          "disk containers were not numbered one after another among themselves");

  // The ids themselves stay unique across every kind: they are the handle the
  // page addresses a container by, and nothing here may collapse two of them.
  std::vector<std::uint32_t> ids;
  for (const auto& status : session.container_statuses()) ids.push_back(status.id);
  std::ranges::sort(ids);
  require(std::ranges::adjacent_find(ids) == ids.end(),
          "container identifiers stopped being unique");
}

void test_a_mount_is_recorded_where_the_driver_put_it() {
  // The session owns the valid drive returned by its runtime. The fake uses
  // the production completion path, including its nonzero result guarantee.
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.partition_password_64 = true;
  CardLoginMountSessionCore session(authentication, runtime, layout());
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.creation_cancelled = [] { return false; };
  runtime.remember_paths = true;
  usb_vault::volume::PartitionCandidate target;
  target.layout = usb_vault::volume::PartitionLayout::mbr;
  target.device_id[0] = 7;
  target.offset_bytes = 1048576;
  target.length_bytes = 64ULL * 1024 * 1024;
  target.logical_sector_bytes = 512;
  const std::uint32_t id = session.create_partition(target, [](const auto&) { return true; });
  session.unmount_container(id);

  require_throws<std::logic_error>([&] { session.set_container_file_hidden(id, true); }, "partition accepted file visibility attributes");
  runtime.partition_lands_on = L'K';
  session.mount_adopted_container(id);
  const auto statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].mounted && statuses[0].drive_letter == L'K',
          "the session recorded the requested drive instead of the mounted one");
  session.unmount_container(id);
  require(!runtime.attached_volume_sizes.contains(L'K'),
          "release addressed a drive the volume was never on");

  // A rejected mount creates no driver attachment. A successful mount must
  // instead return a valid ownership handle, never zero after attaching.
  runtime.partition_mount_fail = true;
  require_throws<std::runtime_error>([&] { session.mount_adopted_container(id); },
      "a rejected mount was recorded anyway");
  require(!session.container_statuses()[0].mounted,
          "a rejected mount was left marked as mounted");
  require(runtime.attached_volume_sizes.empty(), "failed mount left an unowned attachment");
}

void test_partition_pair_session() {
  for (int scenario = 0; scenario < 5; ++scenario) {
    CardAuthenticationSession auth;
    FakeRuntime runtime;
    runtime.partition_password_64 = true;
    runtime.remember_paths = true;
    runtime.creation_cancelled = [] { return false; };
    CardLoginMountSessionCore session(auth, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = 7; target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024; target.logical_sector_bytes = 512;
    const auto decoy = bytes("decoy");
    runtime.fail_index_write = scenario == 2;
    runtime.partition_fail = scenario == 3;
    bool failed = false;
    std::uint32_t id{};
    try { id = session.create_partition(target, [&](const auto&) {return scenario != 1;},
        scenario == 4 ? target.length_bytes : 16ULL * 1024 * 1024, &decoy); }
    catch (...) { failed = true; }
    require(failed == (scenario != 0), "pair session boundary failed");
    require(runtime.partition_written == (scenario == 0), "pair wrote without valid consent and persistence");
    if (scenario) continue;
    require(session.container_statuses().back().mounted, "pair did not mount login volume");
    session.unmount_container(id);
    static_cast<void>(session.open_decoy_maintenance(id, decoy));
    require(session.container_statuses().back().mounted, "pair maintenance did not mount");
    require_throws<std::logic_error>([&] { static_cast<void>(session.decoy_maintenance_space(id)); },
        "partition reached file-only maintenance measurement");
    session.unmount_container(id);
    CardAuthenticationSession next_auth;
    CardLoginMountSessionCore next(next_auth, runtime, layout());
    next.consume_authenticated_login(root_secret(), bytes("login-password"));
    const auto restored = next.container_statuses();
    require(restored.size() == 1 && restored[0].partition, "pair identity not restored");
    next.mount_adopted_container(restored[0].id);
    require(next.container_statuses()[0].mounted, "restored pair did not probe login derivation");
    next.unmount_container(restored[0].id);
  }
}

void test_partition_card_removal_and_drive_conflicts() {
  for (int scenario = 0; scenario < 5; ++scenario) {
    CardAuthenticationSession auth;
    FakeRuntime runtime;
    runtime.partition_password_64 = true;
    runtime.remember_paths = true;
    bool revoked = false;
    runtime.creation_cancelled = [] { return false; };
    runtime.creation_revoked = [&] { return revoked; };
    CardLoginMountSessionCore session(auth, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));
    usb_vault::volume::PartitionCandidate target;
    target.layout = usb_vault::volume::PartitionLayout::mbr;
    target.device_id[0] = 9; target.offset_bytes = 1048576;
    target.length_bytes = 64ULL * 1024 * 1024; target.logical_sector_bytes = 512;
    // Card removal inside the driver mount revokes the run; a user cancel no
    // longer reaches this point once the partition is written.
    if (scenario == 1 || scenario == 2) runtime.after_partition_mount = [&] { revoked = true; };
    runtime.fail_manual_force = scenario == 2;
    if (scenario == 3) runtime.free_drive_letters.clear();
    if (scenario == 4) {
      runtime.other_existing.push_back(L"D:\\existing.hc");
      const auto file = session.adopt_container(L"D:\\existing.hc");
      runtime.partition_password_64 = false;
      runtime.other_size_bytes = 64ULL * 1024 * 1024;
      session.mount_adopted_container(file);
      runtime.partition_password_64 = true;
      runtime.free_drive_letters = {session.container_statuses()[0].drive_letter};
    }
    bool failed = false;
    try { static_cast<void>(session.create_partition(target, [](const auto&) { return true; })); }
    catch (...) { failed = true; }
    require(failed == (scenario != 0), "partition lifecycle failure outcome");
    const auto statuses = session.container_statuses();
    const auto found = std::ranges::find_if(statuses, [](const auto& status) { return status.partition; });
    require(found != statuses.end(), "partition failure lost saved identity");
    require(found->mounted == (scenario == 0 || scenario == 2), "partition cleanup state imprecise");
    if (scenario == 3 || scenario == 4)
      require(std::ranges::find(runtime.events, "mount:partition") == runtime.events.end(), "drive conflict reached driver");
    runtime.fail_manual_force = false;
    auth.clear_authentication();
    static_cast<void>(session.force_unmount_active());
    require(!session.mounted() && runtime.attached_volume_sizes.empty(), "card removal left partition mounted");
    require_throws<std::logic_error>([&] { session.mount_adopted_container(found->id); }, "removed card remounted partition");
  }
}

void test_partition_format_card_session_boundary() {
  using usb_vault::volume::PartitionCandidate;
  // Success, decline, throwing consent, pre-cancel, clear, same-root re-login,
  // missing consent, short KDF, helper failure, late cancellation and reentry.
  for (int scenario = 0; scenario != 14; ++scenario) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.partition_password_64 = scenario != 7;
    CardLoginMountSessionCore session(authentication, runtime, layout());
    if (scenario != 11) session.consume_authenticated_login(root_secret(), bytes("login-password"));
    runtime.events.clear();
    bool cancelled = scenario == 3;
    runtime.creation_cancelled = [&] { return cancelled; };
    if (scenario == 13) runtime.creation_cancelled = {};
    runtime.partition_skip_confirmation = scenario == 6;
    runtime.partition_fail = scenario == 8;
    if (scenario == 9) runtime.after_partition_confirmation = [&] { cancelled = true; };
    PartitionCandidate target;
    target.disk_number = 7;
    target.partition_number = 2;
    target.length_bytes = 64ULL * 1024 * 1024;
    const auto expected = target;
    // The caller's selection changing cannot redirect the immutable operation.
    runtime.before_partition_confirmation = [&] { target.disk_number = 99; };
    int confirmations = 0;
    bool confirmed_expected_target = false;
    std::function<bool(const PartitionCandidate&)> confirm = [&](const auto& frozen) {
      ++confirmations;
      confirmed_expected_target = frozen == expected;
      require(confirmed_expected_target, "partition confirmation target changed");
      if (scenario == 1) return false;
      if (scenario == 2) throw std::runtime_error("dialog failed");
      if (scenario == 4 || scenario == 5) authentication.clear_authentication();
      if (scenario == 5) authentication.establish(root_secret());
      if (scenario == 10) {
        require_throws<std::logic_error>([&] { session.format_partition(target, confirm); },
            "nested partition format was allowed");
      }
      return true;
    };
    if (scenario == 12) confirm = {};
    bool failed = false;
    try { session.format_partition(target, confirm); }
    catch (const std::exception&) { failed = true; }
    const bool success = scenario == 0 || scenario == 10;
    require(failed != success && runtime.partition_written == success,
            "partition session accepted an invalid transition");
    const bool reached_confirmation = scenario != 3 && scenario != 6 &&
        scenario != 7 && scenario != 11 && scenario != 12 && scenario != 13;
    require(confirmations == (reached_confirmation ? 1 : 0),
            "partition confirmation called at the wrong stage");
    require(!reached_confirmation || confirmed_expected_target,
            "caller target mutation redirected partition confirmation");
    if (scenario == 3 || scenario == 11 || scenario == 12 || scenario == 13)
      require(runtime.events.empty(), "invalid session derived or reached the helper");
    for (const auto& event : runtime.events) {
      require(event == "derive:A" || event == "format:partition",
              "partition format used file publication, cleanup, index or mount");
    }
    if (scenario == 1 || scenario == 8) {
      // Failed attempts release the guard; the next attempt needs fresh consent.
      runtime.partition_fail = false;
      session.format_partition(target, [](const auto&) { return true; });
      require(runtime.partition_written, "format failure blocked a fresh attempt");
    }
  }
}

void test_finished_partition_write_is_not_reported_cancelled() {
  using usb_vault::volume_creation::CreationError;
  using usb_vault::volume_creation::make_error_code;
  usb_vault::volume::PartitionCandidate target;
  target.layout = usb_vault::volume::PartitionLayout::mbr;
  target.device_id[0] = 11;
  target.offset_bytes = 1048576;
  target.length_bytes = 64ULL * 1024 * 1024;
  target.logical_sector_bytes = 512;
  const auto confirm = [](const auto&) { return true; };
  // 0: the cancel flag rises right after the write; 1: the page's stage 4
  // gate lost the race and throws a cancellation; 2: the card is revoked
  // after the write, which still stops the mount.
  for (int scenario = 0; scenario < 3; ++scenario) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.partition_password_64 = true;
    runtime.remember_paths = true;
    bool cancelled = false;
    bool revoked = false;
    runtime.creation_cancelled = [&] { return cancelled; };
    runtime.creation_revoked = [&] { return revoked; };
    std::vector<unsigned int> stages;
    runtime.creation_feedback = [&](unsigned int stage, unsigned int, wchar_t) {
      stages.push_back(stage);
      if (stage == 4 && scenario == 1) {
        throw std::system_error(make_error_code(CreationError::operation_cancelled));
      }
    };
    runtime.after_partition_write = [&] {
      cancelled = scenario != 2;
      revoked = scenario == 2;
    };
    CardLoginMountSessionCore session(authentication, runtime, layout());
    session.consume_authenticated_login(root_secret(), bytes("login-password"));

    if (scenario == 0) {
      session.format_partition(target, confirm);
      require(runtime.partition_written && cancelled,
              "a cancel after the write was treated as an unfinished format");
      cancelled = false;
      runtime.partition_written = false;
      runtime.events.clear();
      stages.clear();
    }

    std::error_code failure;
    try {
      static_cast<void>(session.create_partition(target, confirm));
    } catch (const std::system_error& error) {
      failure = error.code();
    }
    const auto statuses = session.container_statuses();
    const bool mounted_now =
        std::ranges::find(runtime.events, "mount:partition") != runtime.events.end();
    require(runtime.partition_written &&
                std::ranges::find(stages, 4U) != stages.end() &&
                statuses.size() == 1 && statuses[0].partition,
            "a finished partition write left the list or skipped stage 4");
    if (scenario == 2) {
      require(failure == make_error_code(CreationError::mount_failed) &&
                  !mounted_now && !statuses[0].mounted,
              "a revoked card still mounted the finished partition");
    } else {
      require(!failure && mounted_now && statuses[0].mounted &&
                  std::ranges::find(stages, 5U) != stages.end(),
              "a cancel after the write stopped or misreported the mount");
    }
  }
}

void test_monitor_reconciliation_preserves_session_and_new_mounts() {
  using usb_vault::volume::MountObservation;
  using usb_vault::volume::MountPresence;
  CardAuthenticationSession auth;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(auth,runtime,layout());
  session.consume_authenticated_login(root_secret(),bytes("login"));
  const auto first=session.create_container(L"C:\\vault\\watch-a.hc",64ULL*1024*1024);
  const auto second=session.create_container(L"C:\\vault\\watch-b.hc",64ULL*1024*1024);
  const auto targets=session.mount_watch_targets();
  require(targets.size()==2 && targets[0].unique_id>=0 && targets[1].unique_id>=0,"mount baselines missing");
  const auto history=session.operation_history().size();
  const auto writes=runtime.index_writes;
  std::vector<MountObservation> observations;
  for(const auto& target:targets) observations.push_back({target.container_id,MountPresence::unknown});
  require(!session.reconcile_mounts(targets,observations),"unknown query released a volume");
  require(session.mount_watch_targets()==targets,"unknown query changed ownership");
  for(auto& observation:observations) if(observation.container_id==first) observation.presence=MountPresence::replaced;
  runtime.events.clear();
  require(session.reconcile_mounts(targets,observations),"external replacement not reconciled");
  require(session.mount_watch_targets().size()==1 && session.mount_watch_targets()[0].container_id==second,"other mount lost");
  require(runtime.events.empty() && runtime.index_writes==writes && session.operation_history().size()==history,"observation mutated disk or history");
  require(auth.authenticated(),"external removal revoked authentication");
  session.mount_adopted_container(first);
  require(!session.reconcile_mounts(targets,observations),"stale observation removed a remount");
  require(session.mount_watch_targets().size()==2,"remount lost");
  observations[0].container_id=999;
  require(!session.reconcile_mounts(targets,observations),"mismatched observation accepted");
}

// A pulled medium: the driver still holds the volume, so the session forces
// it off and releases the row; a failed force keeps the row mounted.
void test_monitor_forces_off_a_volume_whose_medium_is_gone() {
  using usb_vault::volume::MountObservation;
  using usb_vault::volume::MountPresence;
  for (const bool fail : {false, true}) {
    CardAuthenticationSession auth; FakeRuntime runtime;
    CardLoginMountSessionCore session(auth,runtime,layout());
    session.consume_authenticated_login(root_secret(),bytes("login"));
    const auto id=session.create_container(L"C:\\vault\\watch-usb.hc",64ULL*1024*1024);
    const auto targets=session.mount_watch_targets();
    require(targets.size()==1,"mount baseline missing");
    runtime.fail_manual_force=fail;
    const auto attempts=runtime.forced_unmount_attempts;
    const std::vector<MountObservation> observations{{id,MountPresence::backing_lost}};
    require(session.reconcile_mounts(targets,observations)==!fail && session.mounted()==fail &&
            runtime.forced_unmount_attempts==attempts+1,"lost medium was not forced off exactly once");
    require(session.mount_watch_targets().size()==(fail?1u:0u),"failed force released the row");
  }
}

void test_monitor_absence_after_identity_failure() {
  using usb_vault::volume::MountObservation;
  using usb_vault::volume::MountPresence;
  CardAuthenticationSession auth; FakeRuntime runtime;
  CardLoginMountSessionCore session(auth,runtime,layout());
  session.consume_authenticated_login(root_secret(),bytes("login"));
  runtime.fail_identity=true;
  const auto id=session.create_container(L"C:\\vault\\watch-unknown.hc",64ULL*1024*1024);
  const auto targets=session.mount_watch_targets();
  require(targets.size()==1 && targets[0].unique_id==-1,"identity failure lost owned mount");
  const std::vector<MountObservation> observations{{id,MountPresence::missing}};
  require(session.reconcile_mounts(targets,observations) && !session.mounted(),"absence-only observation not applied");
}

void test_monitor_releases_maintenance_facts_without_unmount() {
  using usb_vault::volume::MountObservation;
  using usb_vault::volume::MountPresence;
  CardAuthenticationSession auth; FakeRuntime runtime;
  CardLoginMountSessionCore session(auth,runtime,layout());
  const auto id=maintenance_fixture(runtime,session);
  static_cast<void>(session.open_decoy_maintenance(id,bytes("decoy-password")));
  const auto targets=session.mount_watch_targets();
  require(targets.size()==1 && targets[0].container_id==id,"maintenance missing from watch");
  const std::vector<MountObservation> observations{{id,MountPresence::missing}};
  const auto history=session.operation_history().size(); runtime.events.clear();
  require(session.reconcile_mounts(targets,observations),"maintenance disappearance ignored");
  require(session.mount_watch_targets().empty() && !session.mounted(),"maintenance mount not cleared");
  for(const auto& event:runtime.events) require(!event.starts_with("force:") && !event.starts_with("unmount:"),"monitor unloaded an external occupant");
  require(auth.authenticated() && session.operation_history().size()==history,"maintenance observation reset session or recorded history");
}

}  // namespace

int main() {
  try {
    test_display_alias_persistence_and_export_identity();
    test_protected_creation_shows_only_the_display_name();
    test_alias_save_repairs_missing_file_records_and_restores_imports();
    test_partition_alias_survives_conversion_and_restart();
    test_partition_conversion_lifecycle();
    test_partition_creation_persistence_and_mounting();
    test_a_disk_container_keeps_its_name_through_a_drag_and_a_restart();
    test_a_dragged_order_records_disk_containers_too();
    test_disk_containers_are_numbered_among_themselves();
    test_a_mount_is_recorded_where_the_driver_put_it();
    test_partition_pair_session();
    test_partition_card_removal_and_drive_conflicts();
    test_partition_format_card_session_boundary();
    test_finished_partition_write_is_not_reported_cancelled();
    test_all_container_shapes_establish_authentication();
    test_mount_failures_do_not_revoke_card_authentication();
    test_invalid_or_duplicate_handoff_fails_before_mount();
    test_unmount_state_is_precise_and_forced_order_is_b_then_a();
    test_card_removal_keeps_releasing_after_one_volume_fails();
    test_container_a_creation_requires_authentication_and_valid_size();
    test_file_hiding_publication_and_session_guards();
    test_container_a_creation_publishes_then_mounts();
    test_container_a_creation_rejects_existing_targets();
    test_container_a_creation_cleans_prepublication_failures();
    test_container_a_creation_reports_cleanup_failure();
    test_container_a_mount_failure_keeps_published_container();
    test_container_a_notify_failure_retains_mount_state();
    test_manual_login_adopts_without_deriving_anything();
    test_adopted_containers_mount_without_further_input();
    test_forced_release_drops_the_captured_volume_password();
    test_deliberate_release_keeps_the_captured_volume_password();
    test_operation_selection_does_not_query_attached_volume_capacity();
    test_manual_forced_release_preserves_scope_secrets_and_failed_state();
    test_import_falls_back_to_the_login_derivation();
    test_runtime_log_records_session_stages_and_failures();
    test_import_reports_a_file_no_derivation_opens();
    test_container_b_creation_derives_both_volume_passwords();
    test_container_b_creation_rejects_unusable_input();
    test_container_b_creation_cleans_a_failed_format();
    test_hidden_containers_are_unlimited_and_share_one_derivation();
    test_capacity_reports_the_attached_volume_not_the_file();
    test_one_unreadable_volume_does_not_hide_the_list();
    test_container_a_status_projects_live_file_and_mount_facts();
    test_login_authenticates_without_mounting();
    test_initialization_handoff_supports_hidden_creation();
    test_manual_policy_still_serializes_and_allows_creation();
    test_multiple_standard_containers_get_distinct_drive_letters();
    test_container_paths_are_adopted_once();
    test_unmounted_container_can_be_mounted_again();
    test_batch_items_take_the_batch_unmount_path();
    test_mount_requires_authentication_and_an_existing_file();
    test_unmount_active_releases_every_adopted_container();
    test_operation_history_records_only_committed_session_facts();
    test_operation_logging_can_pause_without_affecting_volume_actions();
    test_creation_feedback_prevents_mount_after_interruption();
    test_a_cancelled_creation_publishes_nothing_and_keeps_no_staging();
    test_both_creation_paths_preserve_cancellation_cleanup_and_retry();
    test_both_creation_paths_do_not_cancel_after_publication();
    test_operation_history_distinguishes_publish_from_mount_failure();
    test_hidden_container_stays_listed_when_its_mount_fails();
    test_operation_history_records_prepublication_and_unmount_failures();
    test_operation_history_discards_only_the_oldest_events_at_capacity();
    test_container_order_persists_and_failed_writes_preserve_session_order();
    test_restored_order_keeps_unmatched_entries_stable_and_preserves_identity();
    test_every_way_a_container_arrives_writes_it_down();
    test_adopting_a_container_lists_it_without_opening_anything();
    test_adopting_refuses_a_file_that_is_not_there();
    test_a_container_already_written_down_is_not_written_again();
    test_forgetting_takes_the_record_and_leaves_the_container();
    test_an_index_that_cannot_be_reached_costs_only_the_memory();
    test_a_login_gets_back_the_containers_whose_files_are_there();
    test_a_remembered_container_is_opened_by_whichever_derivation_answers();
    test_a_login_that_cannot_read_the_index_still_opens();
    test_turning_remembering_off_stops_writing_without_erasing();
    test_dropping_a_record_works_whether_or_not_remembering_is_on();
    test_the_file_manager_reaches_a_container_by_identifier();
    test_the_file_manager_refuses_a_container_it_cannot_reach();
    test_a_path_that_could_leave_the_container_never_reaches_the_disk();
    test_deleting_and_creating_name_the_entry_the_page_picked();
    test_renaming_stays_inside_one_directory();
    test_a_move_within_one_container_renames_and_between_two_carries_bytes();
    test_an_export_leaves_the_volume_and_nothing_records_that_it_did();
    test_selection_export_resolves_vault_groups_and_guards_routes();
    test_an_encrypted_export_leaves_no_trace_and_refuses_containers();
    test_an_encrypted_import_reaches_a_mounted_container_without_a_trace();
    test_a_container_created_for_an_import_is_recorded_only_on_success();
    test_an_export_into_a_container_is_refused_as_a_copy();
    test_an_export_location_that_is_not_one_never_reaches_the_disk();
    test_a_container_file_is_migrated_verbatim_and_nothing_else_moves();
    test_a_mounted_container_is_not_migrated();
    test_a_migration_comes_out_as_a_visible_folder();
    test_a_container_is_deleted_from_disk_only_when_safe();
    test_a_migration_refuses_what_it_cannot_name();
    test_maintenance_opens_the_outer_volume_under_protection();
    test_maintenance_refuses_every_failure_the_same_way();
    test_a_trial_answers_without_leaving_anything_mounted();
    test_maintenance_refuses_before_it_touches_anything();
    test_creation_yields_the_idle_maintenance_helper_without_unmounting();
    test_a_survey_lists_only_what_the_password_opens();
    test_a_survey_counts_containers_it_could_not_release();
    test_a_survey_stops_when_asked_and_keeps_nothing_it_did_not_find();
    test_a_surveyed_container_opens_without_asking_again();
    test_maintenance_protection_query_is_scoped_and_never_invents_safety();
    test_only_a_volume_opened_for_maintenance_is_measured();
    test_measurement_rejects_replaced_mounts();
    test_measurement_requires_live_authentication();
    test_maintenance_identity_failure_releases_new_mount();
    test_ending_maintenance_releases_the_volume_and_the_elevation();
    test_every_unmount_path_ends_a_measuring_run();
    test_multiple_maintenance_mounts_share_one_helper();
    test_a_shell_drop_lands_in_the_directory_the_page_named();
    test_a_shell_drop_reaches_only_a_volume_open_for_maintenance();
    test_a_shell_drop_cannot_name_anything_outside_the_directory();
    test_a_shell_drop_carries_its_cancellation_to_the_storage_layer();
    test_a_shell_drop_needs_an_authenticated_session();
    test_failed_import_latches_until_maintenance_remount();
    test_import_rechecks_identity_before_and_after_copy();
    test_no_export_leaves_a_single_local_trace();
    test_monitor_reconciliation_preserves_session_and_new_mounts();
    test_monitor_forces_off_a_volume_whose_medium_is_gone();
    test_monitor_absence_after_identity_failure();
    test_monitor_releases_maintenance_facts_without_unmount();
    std::cout << "card_login_mount_session_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_login_mount_session_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
