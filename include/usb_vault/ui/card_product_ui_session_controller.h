#pragma once

#include "usb_vault/application/activation_store_v1.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/backup/import_package_v1.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_file_chooser.h"

#include "usb_vault/ui/login_reset_mount_access.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::ui {

inline constexpr std::uint32_t kContainerCreationMinimumMib = 20;
inline constexpr std::uint32_t kContainerCreationMaximumMib = 1024U * 1024U;

// Complete, vendor-neutral page state. It reveals neither hardware identity,
// password role, mode, tier, B-container shape, nor volume type. Unified
// mounted state remains separate for safe cleanup; per-container display facts
// use the narrow status below.
enum class CardProductUiState : std::uint8_t {
  no_card = 0,
  initialization_required = 1,
  login_required = 2,
  authenticated = 3,
  unavailable = 4,
  // The card could initialize or log in, but has no usable activation grant.
  activation_required = 5,
};

// One standard container adopted by the current session, projected for the
// page. The identifier is session scoped, the name is the file name alone,
// and no path, salt, password, tier, or volume type is included.
struct ContainerUiStatus final {
  std::uint32_t id{};
  std::wstring name;
  bool mounted{};
  std::uint32_t size_mib{};
  wchar_t drive_letter{};
  bool partition{};
  std::uint32_t conversion_state{};
  // A mounted row whose volume the driver could not report; size_mib is 0.
  bool volume_unavailable{};
};

// One entry of a directory inside a mounted container, projected for the
// page. The name is a single component and never a path, so nothing a listing
// hands the page can name somewhere else.
struct DirectoryEntryUiStatus final {
  std::wstring name;
  bool is_directory{};
  // A junction, symbolic link, or other reparse point. Marked rather than
  // hidden: a manager that silently omits what Explorer shows is worse than
  // one that says what is there and refuses to follow it.
  bool is_link{};
  // Zero for a directory. A directory's own record size is not what the word
  // "size" means in a file manager.
  std::uint64_t size_bytes{};
  std::int64_t modified_unix_ms{};
};

struct DirectoryUiListing final {
  std::vector<DirectoryEntryUiStatus> entries;
  // Set when the directory held more than one listing carries. What is here is
  // then some of it rather than the first of it by any order the page can see.
  bool truncated{};
};

// What one decoy password found, projected for the page: identifiers only.
// Which container holds what is never part of it.
struct DecoyMaintenanceUiSurvey final {
  std::vector<std::uint32_t> maintainable;
  std::uint32_t unreachable{};
  bool cancelled{};
};

// Byte facts for the current authenticated mount. Safe free excludes protected
// clusters; filesystem metadata can still consume part of this snapshot.
struct DecoyMaintenanceUiSpace final {
  std::uint64_t total_bytes{};
  std::uint64_t free_bytes{};
  std::uint64_t safe_free_bytes{};
};

enum class OperationUiKind : std::uint8_t {
  session_opened = 0,
  container_created = 1,
  container_mounted = 2,
  container_unmounted = 3,
};

enum class OperationUiOutcome : std::uint8_t {
  succeeded = 0,
  failed = 1,
};

// Closed, current-session-only projection for the page. `container_name` is a
// file name, never a path, and no free-form diagnostic field exists.
struct OperationUiEvent final {
  std::uint64_t sequence{};
  std::int64_t completed_at_unix_ms{};
  OperationUiKind kind{OperationUiKind::session_opened};
  OperationUiOutcome outcome{OperationUiOutcome::succeeded};
  std::wstring container_name;
  wchar_t drive_letter{};
};

// Native product UI boundary for P2. Mutable bridge inputs are wiped on every
// path. Initialization performs two independent NFC ingresses before creating
// a confirmation proof; login always accepts exactly one input and delegates
// retry/invalidation semantics to the insertion owner.
class CardProductUiSessionController final {
 public:
  CardProductUiSessionController(
      application::ICardEntryActions& entry,
      application::CardAuthenticationSession& authentication,
      application::IAuthenticatedCardVolumeSession& volume_session,
      const security::IPasswordStrengthPolicy& password_policy,
      IContainerFileChooser& file_chooser,
      ILoginResetMountAccess* reset_mount_access = nullptr,
      application::IActivationGate* activation = nullptr) noexcept;

  // Activation. Without a gate every card counts as activated. The current
  // card is the present insertion before login and the card the session
  // logged in with afterwards; a session that outlives its grant continues.
  [[nodiscard]] std::optional<std::uint32_t> activation_card() const noexcept;
  [[nodiscard]] licensing::ActivationState activation_state() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> activation_expiry() const noexcept;
  // Accepted only on the activation route or inside an authenticated session.
  [[nodiscard]] application::ActivationInstallResult submit_activation_code(
      std::string_view code);

  void submit_login(std::span<std::byte> mutable_utf8);
  void submit_initialization(
      std::span<std::byte> mutable_password_utf8,
      std::span<std::byte> mutable_confirmation_utf8);

  // Opens the native folder picker behind the form's browse button and returns
  // the directory the user chose, or an empty string when they cancel. It only
  // fills a field: what comes back is checked on the way in again, exactly as
  // a typed directory is, so nothing here is trusted for having been picked.
  [[nodiscard]] std::wstring browse_for_container_directory();

  // Creates one standard container of the requested size inside `directory`,
  // shown as `name` without a known container suffix. On disk it is a random
  // GUID file in a random protected folder, built and checked by
  // `resolve_protected_container_target_path`; a rejected name or location, a directory
  // that does not exist, an out-of-range size, or a wrong route all throw
  // `std::invalid_argument`. There is no cancel outcome any more: the user
  // settles every field before the create button commits.
  bool begin_partition_conversion(const volume::PartitionCandidate& target, const std::function<bool(const volume::PartitionCandidate&)>& confirm);
  bool resume_partition_conversion(std::uint32_t id);
  void create_partition(const volume::PartitionCandidate& target, const std::function<bool(const volume::PartitionCandidate&)>& confirm);
  void create_partition_pair(const volume::PartitionCandidate& target, std::uint32_t hidden_size_mib,
      std::span<std::byte> password, std::span<std::byte> confirmation,
      const std::function<bool(const volume::PartitionCandidate&)>& confirm);


  std::uint32_t create_container(std::wstring_view name, std::wstring_view directory,
                                 std::uint32_t size_mib);

  // Creates one hidden-volume container at the same kind of caller-named
  // location and commits the second card password that opens its outer volume.
  // An out-of-range shape, a rejected location, or a wrong route throws. Any
  // number of these containers may exist and they all share one password pair,
  // so the prompt is unconditional and asks for it again every time. The
  // caller supplies only the new password twice; the password the session was
  // opened under comes from the session itself, so it is never retyped and
  // never crosses the page. Both buffers are wiped on every path. The location
  // is resolved and checked before either password is consumed, and the card
  // record is updated after that and before the volume is created, so a failed
  // creation leaves a retryable state instead of a container nobody can open.
  std::uint32_t create_container_b(
      std::wstring_view name, std::wstring_view directory,
      std::uint32_t outer_size_mib, std::uint32_t hidden_size_mib,
      std::span<std::byte> mutable_second_password_utf8,
      std::span<std::byte> mutable_second_confirmation_utf8);

  // Adopts and mounts an existing container the user picks in the native
  // chooser. Returns false when the user cancels.
  bool mount_container();

  // Mounts a container this session already adopted. Nothing is asked of the
  // user: the session still holds everything each container needs to open.
  void mount_container(std::uint32_t id);

  // Tries one decoy password against every container and reports which ones
  // it can open for maintenance. The buffer is wiped on every path, and the
  // password itself is kept by the session rather than by this page.
  [[nodiscard]] std::vector<std::uint32_t> decoy_maintenance_candidates();
  [[nodiscard]] DecoyMaintenanceUiSurvey survey_decoy_maintenance(
      std::span<std::byte> mutable_password_utf8,
      const application::SurveyProgress& progress = {},
      const application::SurveyCancellation& cancelled = {});

  // Opens one container the survey found and returns the drive letter its
  // outer volume came up on. Nothing is asked of the user a second time.
  [[nodiscard]] wchar_t open_decoy_maintenance(std::uint32_t id);

  // Reports filesystem space, not a safe write budget. Bitmap measurement
  // needs consent, which is asked once for a run of maintenance.
  [[nodiscard]] DecoyMaintenanceUiSpace decoy_maintenance_space(
      std::uint32_t id);

  // Closes maintenance: the outer volume is released and the measuring run
  // ends with it.
  [[nodiscard]] bool decoy_maintenance_write_blocked(std::uint32_t id);
  void end_decoy_maintenance(std::uint32_t id);

  void unmount_container(std::uint32_t id);
  void force_unmount_container(std::uint32_t id);
  void unmount_batch_item(std::uint32_t id, bool force);
  void force_unmount_user_active();

  // Takes a container off the list and forgets where it was left. The file
  // stays exactly where it is: this drops the record, never the container, and
  // the same file can be brought back later through the import button. The
  // session refuses a mounted one.
  void forget_container(std::uint32_t id);

  // Lists one directory inside a mounted container, named by the identifier
  // this session handed the page. Every one of these refuses on any route
  // other than `authenticated`, and refuses rather than answering emptily: an
  // empty listing would read as an empty directory.
  //
  // The page never names a drive letter and never assembles an absolute path.
  // It says which container and which directory inside it, and the session
  // turns that into a path that provably cannot leave the volume.
  [[nodiscard]] DirectoryUiListing list_container_directory(
      std::uint32_t id, std::wstring_view relative_directory);

  // Deletes one entry permanently. There is no confirmation here or below it:
  // the page owns whatever it wants to ask before calling this.
  void delete_container_entry(std::uint32_t id,
                              std::wstring_view relative_directory,
                              std::wstring_view name);

  void rename_container_entry(std::uint32_t id,
                              std::wstring_view relative_directory,
                              std::wstring_view name,
                              std::wstring_view new_name);

  void create_container_directory(std::uint32_t id,
                                  std::wstring_view relative_directory,
                                  std::wstring_view name);

  // Moves or copies one entry into a directory in this or another mounted
  // container, keeping its name.
  //
  // ⚠️ Between two containers this reads and writes every byte and cannot be
  // cancelled, and the call is synchronous. Nothing on a page reaches it yet;
  // wiring it to a drag is P3.9's, together with somewhere other than the UI
  // thread to run it.
  void move_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory);
  void copy_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory);

  // Copies one entry the shell handed the host window into the maintenance
  // volume the page is showing. The page names only the container and the
  // directory; the source path comes from the native drop and never crosses
  // the page boundary in either direction.
  void import_external_entry(std::uint32_t id,
                             std::wstring_view relative_directory,
                             std::wstring_view absolute_source,
                             const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress = {});

  // 🔴 Copies one entry out of a mounted container to an ordinary location:
  // the plaintext export. What it writes is no longer protected by anything
  // this product does, and if what was exported came out of a hidden volume,
  // the copy's existence is itself evidence that the hidden volume exists.
  //
  // The page picks the location with the same native chooser the creation form
  // uses, so nothing here is trusted for having been picked: the session
  // checks the location again, and refuses one inside a mounted container.
  void export_container_entry(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name,
      std::wstring_view absolute_destination_directory);

  // Copies one adopted container's own file out, byte for byte: the migration
  // export, for a user moving to another machine. The container must not be
  // mounted, because a copy of a volume the driver is writing to is not a
  // usable one.
  void export_container_file(
      std::uint32_t id, std::wstring_view absolute_destination_directory);
  // The migration as a visible folder named by the page (the container's
  // display name when empty) holding the container file.
  void export_container_folder(std::uint32_t id,
                               std::wstring_view absolute_destination_directory,
                               std::wstring_view folder_name);

  // P5.9 encrypted export. The export password arrives twice in independent
  // buffers, both wiped on every path, and must match and pass the offline
  // password policy before anything is read: it protects a file that can be
  // guessed against offline for as long as it exists.
  void export_container_selection(std::span<const application::ExportSelectionEntry> entries,
      std::wstring_view destination_directory, std::wstring_view folder_name, bool encrypted,
      std::span<std::byte> mutable_password_utf8, std::span<std::byte> mutable_confirmation_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress);
  void export_container_entry_encrypted(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name, std::wstring_view absolute_destination_directory,
      std::wstring_view package_name, std::span<std::byte> mutable_password_utf8,
      std::span<std::byte> mutable_confirmation_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress);

  // P5.9 generic import. One pick serves containers and cipher export
  // packages. A VeraCrypt file container is always a whole number of 512-byte
  // sectors, so any other size is a package. A sector-sized file with a
  // container extension, or named the way this product names new containers
  // ("{GUID}" with no extension), is taken as a container, as before; any
  // other is left for the user to decide. Nothing is read from the file.
  enum class ImportFileKind : unsigned int {
    cancelled = 0,
    container = 1,
    package = 2,
    undecided = 3,
  };
  struct ImportFileChoice final {
    ImportFileKind kind{ImportFileKind::cancelled};
    std::wstring name;
  };
  // Picks a file natively. A container is adopted (or mounted) at once, as the
  // import button always did; a package is held for the import dialog, in
  // memory only and for this login only, and its path never crosses to the
  // page; an undecided file is held until one of the two calls below settles it.
  [[nodiscard]] ImportFileChoice choose_import_file();

  // Deletes an unmounted file container from disk, then from the list.
  void delete_container_from_disk(std::uint32_t id);
  // Settles an undecided pick as a container. False when nothing is held.
  bool adopt_undecided_import_file();
  // Settles an undecided pick as a package and returns its name; empty when
  // nothing is held for this login.
  [[nodiscard]] std::wstring treat_undecided_import_as_package();

  // Copies the selected path for retries within the same login. Another pick
  // or a login change releases it; no password or decrypted data is cached.
  [[nodiscard]] std::wstring selected_import_package();
  // Names what the selected package restores inside the vault. Only that
  // name changes; the package file keeps its own name and place. Throws
  // std::invalid_argument for a name a volume cannot hold and
  // std::logic_error when no package of this login is selected.
  void set_selected_import_name(std::wstring_view name);

  // Thrown when an import into a new container failed and the container it
  // created could not be fully undone: it may still be listed or on disk.
  class ImportRollbackIncomplete final : public std::runtime_error {
   public:
    ImportRollbackIncomplete()
        : std::runtime_error("the container created for a failed import was not fully removed") {}
  };

  // Forgets a picked or verified package that belongs to no current login:
  // its handle closes and the retained password copy is wiped. Called when
  // the session changes, so nothing of an import outlives the login.
  void release_stale_import_state() noexcept;

  // P5.9 import into a new container. A package verified for this login,
  // held open together with a page-locked copy of its password until it is
  // taken or the login ends.
  struct VerifiedImport final {
    explicit VerifiedImport(std::wstring_view package) : package_file(package) {}
    backup::ImportPackageV1 package_file;
    security::SecureBuffer password;
    std::uint64_t generation{};
  };

  // The read-only pass for an import into a new container; runs on the
  // session worker. On success the package stays open and verified here.
  void verify_import(std::wstring_view package, std::span<std::byte> mutable_password_utf8,
                     const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress);

  // Smallest container, in MiB, the verified package fits into; zero when
  // nothing verified belongs to the current login.
  [[nodiscard]] std::uint32_t verified_import_minimum_mib() const;

  // Hands over the verified package, or nothing when it belongs to another
  // login. Forgotten either way.
  [[nodiscard]] std::unique_ptr<VerifiedImport> take_verified_import();

  // Creates a container exactly as the creation form does and restores the
  // verified package into its root. Nothing is recorded unless both succeed;
  // a failure after creation unmounts and deletes the new container. The
  // mounted volume must be at least verified_import_minimum_mib(). Returns
  // the new container's identifier.
  std::uint32_t import_into_new_container(
      std::unique_ptr<VerifiedImport> verified, std::wstring_view name,
      std::wstring_view directory, std::uint32_t size_mib,
      const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress);
  std::uint32_t import_into_new_container_b(
      std::unique_ptr<VerifiedImport> verified, std::wstring_view name,
      std::wstring_view directory, std::uint32_t outer_size_mib, std::uint32_t hidden_size_mib,
      std::span<std::byte> mutable_second_password_utf8,
      std::span<std::byte> mutable_second_confirmation_utf8,
      const std::function<bool()>& cancelled, const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress);

  // Imports a taken package into a directory of a mounted container. The
  // password buffer is wiped on every path.
  std::wstring import_package_into_container(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view package, std::span<std::byte> mutable_password_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress);

  // The settings switches, both of them the same shape. Reading one needs no
  // session, so the page can show them in their real positions before anyone
  // logs in; changing one does, because the change is a session action like
  // any other.
  [[nodiscard]] bool remembering_container_paths();
  void set_remembering_container_paths(bool remember);
  [[nodiscard]] bool mounting_imported_containers();
  void set_mounting_imported_containers(bool mount);

  void unmount();

  [[nodiscard]] CardProductUiState state() const noexcept;
  [[nodiscard]] bool mounted() const noexcept;
  // Live facts for every adopted container, in adoption order. Returns an
  // empty list on every route other than `authenticated`.
  [[nodiscard]] std::vector<ContainerUiStatus> container_statuses();
  [[nodiscard]] std::vector<application::ContainerOperationTarget> batch_targets();
  [[nodiscard]] std::vector<OperationUiEvent> operation_history();
  void clear_operation_history();
  void set_creation_feedback(application::CreationFeedback feedback);
  void set_creation_cancellation(application::CreationCancellation cancelled);
  void set_creation_revocation(application::CreationCancellation revoked);
  [[nodiscard]] bool operation_logging_enabled();
  bool force_unmount_enabled();
  bool hide_new_file_containers();
  void set_hide_new_file_containers(bool value);
  bool container_file_hidden(std::uint32_t id);
  void set_container_file_hidden(std::uint32_t id, bool hidden);
  unsigned int container_extension_preference();
  void set_container_extension_preference(unsigned int value);
  bool auto_mount_enabled();
  void set_auto_mount_enabled(bool enabled);
  void set_force_unmount_enabled(bool enabled);
  void reorder_containers(std::span<const std::uint32_t> ids);
  void rename_container(std::uint32_t id, std::wstring_view name);
  void set_operation_logging_enabled(bool enabled);

  // Reset is offered only inside an authenticated session. The card entry
  // layer performs a new read-only inspection when the action is invoked; no
  // login or initialization context is retained for this capability.
  [[nodiscard]] bool can_reset_card_record() const noexcept;

  // Destroys the authentication key material on the card and returns it to the
  // provisioning marker, then releases everything this session derived from it.
  // Refuses while a volume is still attached: the card is never written while a
  // mount would outlive the key that opens it. Container files are untouched
  // and stay on disk, permanently unopenable.
  void reset_card_record();
  bool can_login_reset() const noexcept;
  bool login_reset_completed() const noexcept { return login_reset_completed_; }
  LoginResetPreparation prepare_login_reset();
  void cancel_login_reset() noexcept;
  bool login_reset_pending(std::uint32_t ticket) const noexcept;
  void force_login_reset_mounts(std::uint32_t ticket, const std::function<bool()>& interrupted = {});
  void confirm_login_reset(std::uint32_t ticket);

 private:
  ILoginResetMountAccess* reset_mount_access_;
  application::IActivationGate* activation_;
  std::optional<std::uint32_t> authenticated_card_;
  std::vector<volume::MountIdentity> reset_mounts_;
  bool reset_mounts_known_{};
  bool login_reset_completed_{};
  application::ICardEntryActions& entry_;
  application::CardAuthenticationSession& authentication_;
  application::IAuthenticatedCardVolumeSession& volume_session_;
  const security::IPasswordStrengthPolicy& password_policy_;
  IContainerFileChooser& file_chooser_;
  std::wstring pending_import_package_;
  std::wstring pending_import_restored_name_;
  std::uint64_t pending_import_generation_{};
  std::wstring undecided_import_file_;
  std::uint64_t undecided_import_generation_{};
  void adopt_or_mount_container_file(const std::wstring& path);
  std::unique_ptr<VerifiedImport> verified_import_;
  // Guards the pick and the verified package, which the session worker sets
  // and the window thread may release.
  mutable std::mutex import_mutex_;
  template <typename Create>
  std::uint32_t import_into_created(std::unique_ptr<VerifiedImport> verified,
                                    std::uint32_t mounted_mib, Create&& create,
                                    const std::function<bool()>& cancelled,
                                    const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress);
};

}  // namespace usb_vault::ui
