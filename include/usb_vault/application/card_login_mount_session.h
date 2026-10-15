#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/volume/volume_directory_listing.h"
#include "usb_vault/volume/partition_catalog.h"
#include "usb_vault/volume/mount_state_monitor.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::backup {
class ImportPackageV1;
}  // namespace usb_vault::backup

namespace usb_vault::application {

// Session facts for selecting operations; no driver capacity query, path or type.
struct ExportSelectionEntry final {
  std::uint32_t id{};
  std::wstring directory;
  std::wstring name;
};

struct ContainerOperationTarget final {
  std::uint32_t id{};
  std::wstring name;
  bool mounted{};
};

// Presentation-only stages: authorization, allocation, formatting, mounting.
// No container shape, password role or path is carried across this boundary.
// Stage 4 is emitted before publication, within the staging cleanup scope.
// Its callback may reject publication (cancellation or session interruption).
using CreationFeedback = std::function<void(unsigned int, unsigned int, wchar_t)>;

// Whether the creation in progress has been called off. Asked on the thread
// running the creation, and answered without reference to anything the page
// holds; an empty function means this session cannot be cancelled. A cancel
// accepted before publication removes the staging file even if formatting is
// already complete. The UI closes its cancellation gate at stage 4.
using CreationCancellation = std::function<bool()>;

class CardAuthenticationSession;

struct ContainerAStatus final {
  bool exists{};
  bool mounted{};
  std::uint64_t size_bytes{};
  wchar_t drive_letter{};
};

// One standard container adopted by the current authenticated session. The
// identifier is session scoped and never persisted, and only the file name
// crosses this boundary so the UI can tell two containers apart without the
// full path, the salt, the password, or any tier or volume-type fact leaving
// the session.
struct ContainerStatus final {
  std::uint32_t id{};
  std::wstring name;
  bool mounted{};
  std::uint64_t size_bytes{};
  wchar_t drive_letter{};
  bool partition{};
  std::uint32_t conversion_state{}; // 0 ready, 1 incomplete/uncertain, 2 durably paused.
  // Mounted, but the driver could not report the volume this time. The size
  // is then zero; the drive letter is still the one the session holds.
  bool volume_unavailable{};
};

// One best-effort, process-local fact from the current authenticated session.
// The schema is intentionally closed: no free-form detail, full path, card
// identity, password role, container shape, volume type, or export activity can
// enter the history through this boundary.
enum class OperationKind : std::uint8_t {
  session_opened = 0,
  container_created = 1,
  container_mounted = 2,
  container_unmounted = 3,
};

enum class OperationOutcome : std::uint8_t {
  succeeded = 0,
  failed = 1,
};

inline constexpr std::size_t kOperationHistoryCapacity = 128;

struct OperationEvent final {
  std::uint64_t sequence{};
  std::int64_t completed_at_unix_ms{};
  OperationKind kind{OperationKind::session_opened};
  OperationOutcome outcome{OperationOutcome::succeeded};
  // File name only. Empty only for `session_opened`.
  std::wstring container_name;
  // Present for a confirmed mount or an attempted/completed unmount only.
  wchar_t drive_letter{};
};

class ContainerCreationCleanupError final : public std::runtime_error {
 public:
  ContainerCreationCleanupError(std::exception_ptr operation_failure,
                                std::exception_ptr cleanup_failure);

  [[nodiscard]] const std::exception_ptr& operation_failure() const noexcept;
  [[nodiscard]] const std::exception_ptr& cleanup_failure() const noexcept;

 private:
  std::exception_ptr operation_failure_;
  std::exception_ptr cleanup_failure_;
};

// Snapshot, not a guaranteed file payload allowance. Protected free bytes are
// whole free clusters before the authenticated boundary. Measurement failures
// throw instead of returning an old value. The legacy tail field stays zero.
struct DecoyMaintenanceSpace final {
  std::uint64_t total_bytes{};
  std::uint64_t free_bytes{};
  std::uint64_t tail_free_bytes{};
  std::uint64_t safe_free_bytes{};
};

// What one decoy password found across every container the session holds.
// Containers that answered are the only ones a caller may offer to open.
struct DecoyMaintenanceSurvey final {
  std::vector<std::uint32_t> maintainable;
  // Containers that could not be released, so nothing was learned about them.
  // They are not on the list above and the count is what says so.
  std::uint32_t unreachable{};
  bool cancelled{};
};

// Asked between containers; true stops the walk where it stands.
using SurveyCancellation = std::function<bool()>;
// How many containers have been tried, out of how many will be.
using SurveyProgress = std::function<void(std::uint32_t, std::uint32_t)>;

// Every failed attempt at decoy maintenance, in one type with one message:
// telling a wrong password from a container with no hidden volume would answer
// the one question the product must not answer about a container.
class DecoyMaintenanceRefused final : public std::runtime_error {
 public:
  DecoyMaintenanceRefused();
};

// Combined product boundary required by the native UI: a successful login
// handoff and the lifecycle of any resulting active volumes must be the same
// session object. It still exposes no P1 raw-card authentication entry point.
class IAuthenticatedCardVolumeSession : public ICardLoginSuccessConsumer,
                                        public IActiveVolumeSession {
 public:
  ~IAuthenticatedCardVolumeSession() noexcept override = default;

  IAuthenticatedCardVolumeSession(
      const IAuthenticatedCardVolumeSession&) = delete;
  IAuthenticatedCardVolumeSession& operator=(
      const IAuthenticatedCardVolumeSession&) = delete;
  IAuthenticatedCardVolumeSession(
      IAuthenticatedCardVolumeSession&&) = delete;
  IAuthenticatedCardVolumeSession& operator=(
      IAuthenticatedCardVolumeSession&&) = delete;

  // Creates the independent standard A container for the authenticated card,
  // publishes it without replacement, and mounts it before returning. Root
  // bytes and derived passwords never cross this boundary.
  virtual void create_container_a(std::uint64_t size_bytes) = 0;

  // Creates an independent standard container at the caller-selected path,
  // publishes it without replacement, mounts it on a freshly allocated drive
  // letter, and adopts it into the session. Every standard container derives
  // its volume password from the card root alone, so the returned identifier
  // is the only handle the caller needs afterwards.
  virtual std::uint32_t create_container(std::wstring_view path,
                                         std::uint64_t size_bytes) = 0;

  // Native-only destructive format, not adoption/mounting or in-place encryption.
  // Confirmation receives the frozen target after elevated preflight. The caller
  // must serialize session access and supply physical-card cancellation.
  virtual void format_partition(
      const volume::PartitionCandidate&,
      const std::function<bool(const volume::PartitionCandidate&)>&) {
    throw std::logic_error("partition formatting is unavailable");
  }

  virtual std::uint32_t create_partition_pair(const volume::PartitionCandidate&,
      std::uint64_t, const security::SecureBuffer&,
      const std::function<bool(const volume::PartitionCandidate&)>&) {
    throw std::logic_error("partition pair creation unavailable");
  }

  virtual bool begin_partition_conversion(const volume::PartitionCandidate&,
      const std::function<bool(const volume::PartitionCandidate&)>&) { throw std::logic_error("conversion unavailable"); }
  virtual bool resume_partition_conversion(std::uint32_t) { throw std::logic_error("conversion unavailable"); }
  virtual std::uint32_t create_partition(const volume::PartitionCandidate&,
      const std::function<bool(const volume::PartitionCandidate&)>&) {
    throw std::logic_error("partition creation unavailable");
  }

  // Creates an independent outer+hidden container at the caller-selected
  // path, publishes it without replacement, and mounts its hidden volume on a
  // freshly allocated drive letter. The outer volume opens with the decoy
  // password supplied here; the hidden volume opens with the password this
  // session was opened under, which the session still holds and the caller no
  // longer has to produce. Any number of them may exist, and every one of them
  // shares that single pair of passwords: the card record carries exactly two
  // envelopes, so a decoy password per container would leak how many there
  // are.
  virtual std::uint32_t create_container_b(
      std::wstring_view path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8) = 0;

  // Product creation. Same contracts as the two calls above, except that the
  // parent folder of `path` must not exist yet: the session creates it with
  // Hidden and System for this container alone, removes it again if the
  // container is never published, and shows the container as `display_name`
  // (in the list, the volume label, operation records and, when container
  // paths are remembered, the encrypted index). The file name stays private.
  virtual std::uint32_t create_protected_container(std::wstring_view,
      std::uint64_t, std::wstring_view) {
    throw std::logic_error("protected container creation unavailable");
  }
  virtual std::uint32_t create_protected_container_b(std::wstring_view,
      std::uint64_t, std::uint64_t, const security::SecureBuffer&,
      std::wstring_view) {
    throw std::logic_error("protected container creation unavailable");
  }

  // Returns a copy of the password this session was opened under. It exists
  // for the single operation that needs the password itself rather than
  // something derived from it: rewriting the card record so a second password
  // opens it too. The session neither knows nor records which of the two
  // passwords this is, so the copy is symmetric under both logins. An
  // unauthenticated session returns an empty buffer.
  [[nodiscard]] virtual security::SecureBuffer copy_session_password() = 0;

  // Only after successful initialization with this independently confirmed,
  // normalized input. Completes a fresh authenticated session without mounting
  // or touching the card. Consumes the buffer into the existing locked storage.
  virtual void complete_initialization(security::SecureBuffer password_utf8) {
    password_utf8.clear();
    throw std::logic_error("initialization session handoff unsupported");
  }

  // Adopts and mounts an existing container at the caller-selected path. The
  // standard derivation is attempted first and the login-input derivation
  // second, so both container classes are reached through one entry point and
  // the caller never states which class it picked. The session refuses a path
  // it already has mounted.
  virtual std::uint32_t mount_container(std::wstring_view path) = 0;

  // Mounts a container this session already adopted, without asking the user
  // for anything. Standard containers derive from the card alone;
  // hidden-volume containers all share the one volume password derived from
  // the login input, so which of a container's two volumes appears still
  // follows the password the session was opened with.
  virtual void mount_adopted_container(std::uint32_t id) = 0;

  // Opens one container's outer volume under driver protection and returns its
  // drive letter. The password here opens the outer volume; the hidden one is
  // opened by the login input the session still holds. A mounted container is
  // released first, forcibly if busy, and stays released even on a refusal.
  [[nodiscard]] virtual wchar_t open_decoy_maintenance(
      std::uint32_t, const security::SecureBuffer&) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Whether the call above would succeed, leaving nothing mounted either way.
  // False is every refusal it throws; a container that will not release still
  // throws, because that is not an answer about the password.
  [[nodiscard]] virtual bool verify_decoy_maintenance(
      std::uint32_t, const security::SecureBuffer&) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Tries one decoy password against every adopted container and reports the
  // ones whose outer volume it opens, leaving nothing mounted.
  //
  // A container already mounted is released first, forcibly if busy, except a
  // standard one: those have no outer volume to open, so a working disk is
  // never taken away to learn what is already known. The password is derived
  // once for the whole walk, and kept for the rest of the session when at
  // least one container answered.
  [[nodiscard]] virtual std::vector<std::uint32_t> decoy_maintenance_candidates() {
    throw std::logic_error("decoy maintenance is unavailable");
  }
  [[nodiscard]] virtual DecoyMaintenanceSurvey survey_decoy_maintenance(
      const security::SecureBuffer&, const SurveyProgress& = {},
      const SurveyCancellation& = {}) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Reports filesystem space, not a safe outer-volume write budget.
  // Bitmap measurement asks for consent once per run of maintenance.
  [[nodiscard]] virtual DecoyMaintenanceSpace decoy_maintenance_space(
      std::uint32_t) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Releases the container maintenance was done on and ends the measuring run
  // it opened. A container still in use is refused, as any unmount is.
  [[nodiscard]] virtual bool decoy_maintenance_write_blocked(std::uint32_t) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  virtual void end_decoy_maintenance(std::uint32_t) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Copies one file or directory the shell handed over into the current
  // directory of an outer volume already open for maintenance. The source is
  // an ordinary OS path, but the destination is still named only by container
  // id and a checked relative directory, and nothing about the source is kept.
  // The cancellation is asked repeatedly while the copy runs.
  virtual void import_external_entry(std::uint32_t, std::wstring_view,
                                     std::wstring_view,
                                     const std::function<bool()>&, const std::function<void(unsigned int)>& = {}) {
    throw std::logic_error("external import is unavailable");
  }

  // Opens a container the survey found, with the password the survey kept.
  // Refuses the same way as the call taking a password, and throws when this
  // session has no surveyed password to use.
  [[nodiscard]] virtual wchar_t open_surveyed_maintenance(std::uint32_t) {
    throw std::logic_error("decoy maintenance is unavailable");
  }

  // Unmounts one adopted container. The container stays adopted and unmounted
  // so it can be mounted again without reselecting its file.
  virtual void unmount_container(std::uint32_t id) = 0;
  // Explicit user cleanup retains the authenticated session for remounting.
  // Implementations without this capability fail closed.
  virtual void force_unmount_container(std::uint32_t) {
    throw std::logic_error("manual forced cleanup is unavailable");
  }
  // One item of a batch: a busy volume is retried a little longer, and the
  // caller refreshes This PC once after the last item.
  virtual void unmount_batch_item(std::uint32_t id, bool force) {
    if (force) force_unmount_container(id);
    else unmount_container(id);
  }
  virtual void force_unmount_user_active() {
    throw std::logic_error("manual forced cleanup is unavailable");
  }

  // Drops one adopted container from the session and forgets where it is. The
  // file is left exactly where it was: this removes the record, never the
  // container. A mounted container is refused, because a drive letter it left
  // behind would belong to nothing the session can still name.
  virtual void forget_container(std::uint32_t id) = 0;

  // Takes a container into the session without mounting it: the file is
  // checked for and its path written down, and it joins the list unmounted,
  // knowing no more about which password opens it than a path read back from
  // the index does. Importing arrives here unless the user asked for the
  // mount, so nothing is opened on the strength of a file picker alone.
  virtual std::uint32_t adopt_container(std::wstring_view path) = 0;

  // Whether an imported container should be mounted at once, and the switch
  // that changes it. Off is the product default, and it is the reason the
  // call above exists.
  [[nodiscard]] virtual bool mounting_imported_containers() = 0;
  virtual void set_mounting_imported_containers(bool mount) = 0;

  // Whether the product writes container paths down at all, and the switch
  // that changes it. Turning it off stops new paths being recorded and stops
  // the list coming back at the next login; it does not delete what is already
  // recorded, so turning it back on returns the same list. Dropping a
  // container from the list still clears its record either way: that is an
  // explicit instruction rather than a policy.
  [[nodiscard]] virtual bool remembering_container_paths() = 0;
  virtual void set_remembering_container_paths(bool remember) = 0;

  // Lists one directory inside a container this session has mounted.
  //
  // 🔴 A container is named by its session identifier and never by a drive
  // letter. A caller able to name a letter could name any letter on the
  // machine, including the system disk; an identifier reaches only a container
  // this session opened, and the session is the one thing that knows which
  // letter that is. No drive letter is accepted by any call below, and none of
  // them will read one out of a path.
  //
  // `relative_directory` is relative to the volume root, which is what an
  // empty string names. It is checked by `resolve_volume_entry_path` before it
  // reaches the file system, so nothing the caller can say leaves the
  // container as a string; a junction inside the volume that points out of it
  // is refused by the storage layer when it opens the directory. Both halves
  // of "out of the volume is refused" are therefore covered, and neither is
  // this layer's own work.
  //
  // Throws `std::logic_error` when the session is not authenticated or the
  // container is adopted but not mounted, `std::invalid_argument` for an
  // identifier no container answers to or a relative path that is not one, and
  // whatever the storage layer throws for what the file system refused.
  [[nodiscard]] virtual volume::VolumeDirectoryListing list_container_directory(
      std::uint32_t id, std::wstring_view relative_directory) = 0;

  // Deletes one entry, permanently and with no confirmation at this layer.
  // `name` is one component of `relative_directory`, checked as the single
  // name it claims to be: a separator inside it would otherwise turn a delete
  // of the entry the user picked into a delete of something else.
  virtual void delete_container_entry(std::uint32_t id,
                                      std::wstring_view relative_directory,
                                      std::wstring_view name) = 0;

  // Renames one entry in place. Both names are checked as single components,
  // so a rename can never become a move.
  virtual void rename_container_entry(std::uint32_t id,
                                      std::wstring_view relative_directory,
                                      std::wstring_view name,
                                      std::wstring_view new_name) = 0;

  // Creates one directory inside `relative_directory`. The parent must exist:
  // this makes the directory the user just named and never a chain of them.
  virtual void create_container_directory(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name) = 0;

  // Moves one entry to a directory in this or another mounted container,
  // keeping its name. Both containers must be mounted, and an entry already
  // called that at the destination is never replaced.
  //
  // Two identifiers are two containers, and this session never mounts two
  // containers on one letter, so the identifiers alone settle whether this is
  // a rename inside one volume or a copy-and-delete between two -- the letters
  // never have to be compared, and a caller never states which it wanted.
  //
  // ⚠️ A move between containers reads and writes every byte and cannot be
  // cancelled through this boundary yet; the storage layer takes a
  // cancellation but nothing here can produce one until the UI has somewhere
  // to run the transfer other than the calling thread.
  virtual void move_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory) = 0;

  // Copies one entry into a directory in this or another mounted container,
  // under the same rules and with the same caveat about cancellation.
  virtual void copy_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory) = 0;

  // 🔴 Copies one entry out of a mounted container to an ordinary location on
  // this machine. This is the plaintext export: what it writes is no longer
  // protected by anything this product does.
  //
  // It is a separate entry point from `copy_container_entry` rather than a
  // flag on it. The file manager's calls name both ends by container
  // identifier, so they structurally cannot leave a volume; this one takes a
  // location, and its name says what that means.
  //
  // `absolute_destination_directory` must be drive-absolute and must not be on
  // a drive this session has a container mounted on. Copying into a container
  // is the file manager's job and carries none of this one's consequences, so
  // it is refused here rather than quietly performed.
  //
  // 🔴 Nothing about the call is written down: not the location, not that it
  // happened, and not into the operation history. Q-04 makes that this
  // product's obligation rather than the user's risk to carry -- what the user
  // accepted is that the copy may be found, never that this product would
  // record that they made one.
  //
  // The two confirmations Q-04 requires live above this boundary. This call
  // performs an export that has already been agreed to; it does not ask.
  virtual void export_container_entry(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name,
      std::wstring_view absolute_destination_directory) = 0;

  // Copies one adopted container's own file, byte for byte, to an ordinary
  // location. This is the migration export: it is what a user takes to another
  // machine, where the same card and the same password open it again.
  //
  // Nothing is decrypted and nothing is re-encrypted. What comes out is the
  // file that went in, which is the whole reason it carries no root secret, no
  // password, no derivation, and no sign of whether a hidden volume is inside
  // it: adding any of those would be building a key backup, and this product
  // has none. A lost card stays a lost vault.
  //
  // 🔴 The container must not be mounted. The driver is writing to a mounted
  // volume, so a copy taken then is a copy of something in flight -- crash
  // consistent at best. Q-04 forbids handing back an incomplete copy as a
  // usable migration result, so this refuses rather than producing one and
  // hoping.
  //
  // The destination keeps the container's own file name and is never
  // replaced; an occupied name fails. Unlike the plaintext export above, a
  // location inside another mounted container is allowed: a container stored
  // inside a container is still encrypted, so none of what makes an export
  // dangerous applies.
  //
  // Nothing about the call is written down, for the same reason as above.
  virtual void export_container_file(
      std::uint32_t id,
      std::wstring_view absolute_destination_directory) = 0;

  // The migration export as the product offers it: an ordinary folder named
  // `folder_name` (the container's display name when empty) at the location,
  // holding the container file under its own name. Neither carries the
  // Hidden or System attribute the container may have had where it lived.
  // The folder must not exist yet; a failure removes what this created.
  virtual void export_container_folder(std::uint32_t, std::wstring_view,
                                       std::wstring_view) {
    throw std::logic_error("container migration is unavailable");
  }

  // P5.9 encrypted export. Packs one entry of a mounted container into a
  // package that only this product opens, and only with `password_utf8`, at
  // `absolute_destination_directory` under `package_name`, which must not be
  // taken yet. The location rules are the plaintext export's: drive-absolute
  // and not on a drive a container of this session is mounted on.
  //
  // The package carries the entry's own name and contents and nothing about
  // the container, its kind, or the password the session was opened with.
  // Nothing about the call is written down, for the same reason as the two
  // exports above, and a failure leaves no partial package behind.
  virtual void export_container_selection(
      std::span<const ExportSelectionEntry> entries, std::wstring_view destination_directory,
      std::wstring_view folder_name, bool encrypted, const security::SecureBuffer& password,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    static_cast<void>(entries); static_cast<void>(destination_directory);
    static_cast<void>(folder_name); static_cast<void>(encrypted); static_cast<void>(password);
    static_cast<void>(cancelled); static_cast<void>(progress);
    throw std::logic_error("selection export is unavailable");
  }

  virtual void export_container_entry_encrypted(
      std::uint32_t, std::wstring_view, std::wstring_view, std::wstring_view,
      std::wstring_view, const security::SecureBuffer&,
      const std::function<bool()>&,
      const std::function<void(std::uint64_t, std::uint64_t)>&) {
    throw std::logic_error("encrypted export is unavailable");
  }

  // P5.9 encrypted import into a directory of a container mounted normally
  // (not open for decoy maintenance). The whole package is authenticated
  // before anything is written; the entry is then restored under a hidden
  // staging name and renamed into place without replacing anything, and a
  // failure removes only what this import wrote. Returns the restored name.
  // Nothing about the call is written down.
  virtual std::wstring import_package_into_container(
      std::uint32_t, std::wstring_view, std::wstring_view,
      const security::SecureBuffer&, const std::function<bool()>&,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) {
    throw std::logic_error("encrypted import is unavailable");
  }

  // P5.9 import into a container created for it. The write pass of an already
  // verified package into a directory of a normally mounted container.
  virtual std::wstring write_verified_import(
      std::uint32_t, std::wstring_view, backup::ImportPackageV1&,
      const security::SecureBuffer&, const std::function<bool()>&,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>&) {
    throw std::logic_error("encrypted import is unavailable");
  }
  // From defer to settle, what the session would add to the operation
  // history is held back; settling keeps it or drops it. A container created
  // for an import that then fails is never recorded.
  virtual void defer_operation_records() {
    throw std::logic_error("record deferral is unavailable");
  }
  virtual void settle_operation_records(bool) {}
  // Undoes a container this session just created: unmounts it, drops it from
  // the session and the index, and deletes its file and protected folder.
  virtual void discard_new_container(std::uint32_t) {
    throw std::logic_error("container discard is unavailable");
  }
  // Deletes an unmounted file container from disk for good: its file goes
  // first, then the empty folder the product made for it, and only then the
  // list entry and its index record. Not for partitions, not while mounted,
  // and not when the file is not where the list says, since that may be a
  // drive that is simply not attached. Nothing goes to the recycle bin and
  // nothing is recorded.
  virtual void delete_container_from_disk(std::uint32_t) {
    throw std::logic_error("container deletion is unavailable");
  }

  // Returns the live, non-secret facts for every container adopted by this
  // session, in adoption order. Nothing is persisted between sessions.
  [[nodiscard]] virtual std::vector<ContainerStatus> container_statuses() = 0;
  [[nodiscard]] virtual std::vector<ContainerOperationTarget> container_operation_targets() {
    std::vector<ContainerOperationTarget> targets;
    for (const auto& entry : container_statuses())
      targets.push_back({entry.id, entry.name, entry.mounted});
    return targets;
  }

  // Returns only the live, non-secret A-container facts required by the
  // authenticated product UI. No path, password, root, tier, B-container
  // shape, or historical state crosses this boundary.
  [[nodiscard]] virtual ContainerAStatus container_a_status() = 0;

  // Returns a bounded snapshot of the current authenticated session only.
  // Nothing is persisted, and opening a new session replaces the old history.
  [[nodiscard]] virtual std::vector<OperationEvent> operation_history() = 0;
  virtual void clear_operation_history() noexcept = 0;
  virtual void set_creation_feedback(CreationFeedback) {}
  virtual void set_creation_cancellation(CreationCancellation) {}
  virtual void set_creation_revocation(CreationCancellation) {}
  [[nodiscard]] virtual bool operation_logging_enabled() { return true; }
  virtual bool force_unmount_enabled() { return false; }
  virtual bool hide_new_file_containers() { return false; }
  virtual void set_hide_new_file_containers(bool) { throw std::logic_error("preferences unavailable"); }
  virtual bool container_file_hidden(std::uint32_t) { throw std::logic_error("file visibility unavailable"); }
  virtual void set_container_file_hidden(std::uint32_t, bool) { throw std::logic_error("file visibility unavailable"); }
  virtual unsigned int container_extension_preference() { return 0; }
  virtual void set_container_extension_preference(unsigned int) { throw std::logic_error("preferences unavailable"); }
  virtual bool auto_mount_enabled() { return false; }
  virtual void set_auto_mount_enabled(bool) { throw std::logic_error("preferences unavailable"); }
  virtual void set_force_unmount_enabled(bool) { throw std::logic_error("preferences unavailable"); }
  virtual void reorder_containers(std::span<const std::uint32_t>) { throw std::logic_error("ordering unavailable"); }
  virtual void rename_container(std::uint32_t, std::wstring_view) { throw std::logic_error("container naming unavailable"); }
  virtual void set_operation_logging_enabled(bool) {
    throw std::runtime_error("operation logging preference is unavailable");
  }

  // Releases every secret this session retained: the input it was opened
  // under and anything derived from it. The card identity they came from no
  // longer exists, so nothing may keep operating on them. Adopted containers
  // stay in the list, unopenable, which is exactly what they now are.
  virtual void release_session_secrets() noexcept = 0;

 protected:
  IAuthenticatedCardVolumeSession() noexcept = default;
};

// P2 Card Record login success handler. It establishes volatile card
// authentication before attempting any post-authentication volume work, never
// reads the card again, and retains the root, the login input, and unified
// active-volume state. The login input is retained because rewriting the card
// record needs the password itself as wrapping material; it is released on the
// card-removal path together with every other session secret.
class CardLoginMountSession final : public IAuthenticatedCardVolumeSession {
 public:
  CardLoginMountSession(CardAuthenticationSession& authentication,
                        const volume::VolumeProvider& provider,
                        ContainerLayout layout,
                        std::filesystem::path volume_creator_helper_path);
  ~CardLoginMountSession() noexcept override;

  CardLoginMountSession(const CardLoginMountSession&) = delete;
  CardLoginMountSession& operator=(const CardLoginMountSession&) = delete;
  CardLoginMountSession(CardLoginMountSession&&) = delete;
  CardLoginMountSession& operator=(CardLoginMountSession&&) = delete;

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8) override;
  bool begin_partition_conversion(const volume::PartitionCandidate& target,
      const std::function<bool(const volume::PartitionCandidate&)>& confirm) override;
  bool resume_partition_conversion(std::uint32_t id) override;
  std::uint32_t create_partition(const volume::PartitionCandidate& target,
      const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase) override;
  std::uint32_t create_partition_pair(const volume::PartitionCandidate& target,
      std::uint64_t hidden_bytes, const security::SecureBuffer& decoy,
      const std::function<bool(const volume::PartitionCandidate&)>& confirm) override;
  void create_container_a(std::uint64_t size_bytes) override;
  void format_partition(const volume::PartitionCandidate& target,
      const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase) override;
  std::uint32_t create_container(std::wstring_view path,
                                 std::uint64_t size_bytes) override;
  std::uint32_t create_container_b(
      std::wstring_view path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8) override;
  std::uint32_t create_protected_container(std::wstring_view path,
      std::uint64_t size_bytes, std::wstring_view display_name) override;
  std::uint32_t create_protected_container_b(std::wstring_view path,
      std::uint64_t outer_size_bytes, std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8,
      std::wstring_view display_name) override;
  [[nodiscard]] security::SecureBuffer copy_session_password() override;
  void complete_initialization(security::SecureBuffer password_utf8) override;
  std::uint32_t mount_container(std::wstring_view path) override;
  void mount_adopted_container(std::uint32_t id) override;
  [[nodiscard]] wchar_t open_decoy_maintenance(
      std::uint32_t id,
      const security::SecureBuffer& decoy_password_utf8) override;
  [[nodiscard]] bool verify_decoy_maintenance(
      std::uint32_t id,
      const security::SecureBuffer& decoy_password_utf8) override;
  [[nodiscard]] std::vector<std::uint32_t> decoy_maintenance_candidates() override;
  [[nodiscard]] DecoyMaintenanceSurvey survey_decoy_maintenance(
      const security::SecureBuffer& decoy_password_utf8,
      const SurveyProgress& progress,
      const SurveyCancellation& cancelled) override;
  [[nodiscard]] wchar_t open_surveyed_maintenance(std::uint32_t id) override;
  [[nodiscard]] DecoyMaintenanceSpace decoy_maintenance_space(
      std::uint32_t id) override;
  [[nodiscard]] bool decoy_maintenance_write_blocked(std::uint32_t id) override;
  void end_decoy_maintenance(std::uint32_t id) override;
  void import_external_entry(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view absolute_source,
      const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress = {}) override;
  void unmount_container(std::uint32_t id) override;
  void force_unmount_container(std::uint32_t id) override;
  void unmount_batch_item(std::uint32_t id, bool force) override;
  void force_unmount_user_active() override;
  void forget_container(std::uint32_t id) override;
  std::uint32_t adopt_container(std::wstring_view path) override;
  [[nodiscard]] bool mounting_imported_containers() override;
  void set_mounting_imported_containers(bool mount) override;
  [[nodiscard]] bool remembering_container_paths() override;
  void set_remembering_container_paths(bool remember) override;
  [[nodiscard]] volume::VolumeDirectoryListing list_container_directory(
      std::uint32_t id, std::wstring_view relative_directory) override;
  void delete_container_entry(std::uint32_t id,
                              std::wstring_view relative_directory,
                              std::wstring_view name) override;
  void rename_container_entry(std::uint32_t id,
                              std::wstring_view relative_directory,
                              std::wstring_view name,
                              std::wstring_view new_name) override;
  void create_container_directory(std::uint32_t id,
                                  std::wstring_view relative_directory,
                                  std::wstring_view name) override;
  void move_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory) override;
  void copy_container_entry(
      std::uint32_t source_id, std::wstring_view source_relative_directory,
      std::wstring_view name, std::uint32_t destination_id,
      std::wstring_view destination_relative_directory) override;
  void export_container_entry(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name,
      std::wstring_view absolute_destination_directory) override;
  void export_container_file(
      std::uint32_t id,
      std::wstring_view absolute_destination_directory) override;
  void export_container_folder(std::uint32_t id,
                               std::wstring_view absolute_destination_directory,
                               std::wstring_view folder_name) override;
  void export_container_selection(
      std::span<const ExportSelectionEntry> entries, std::wstring_view destination_directory,
      std::wstring_view folder_name, bool encrypted, const security::SecureBuffer& password,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) override;
  void export_container_entry_encrypted(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name, std::wstring_view absolute_destination_directory,
      std::wstring_view package_name, const security::SecureBuffer& password_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(std::uint64_t, std::uint64_t)>& progress) override;
  std::wstring import_package_into_container(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view absolute_package, const security::SecureBuffer& password_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) override;
  std::wstring write_verified_import(
      std::uint32_t id, std::wstring_view relative_directory,
      backup::ImportPackageV1& package, const security::SecureBuffer& password_utf8,
      const std::function<bool()>& cancelled,
      const std::function<void(unsigned int, std::uint64_t, std::uint64_t)>& progress) override;
  void defer_operation_records() override;
  void settle_operation_records(bool keep) override;
  void discard_new_container(std::uint32_t id) override;
  void delete_container_from_disk(std::uint32_t id) override;
  [[nodiscard]] std::vector<ContainerStatus> container_statuses() override;
  // Native owner only, never concurrent with session workers. No driver IO.
  [[nodiscard]] std::vector<volume::WatchedMount> mount_watch_targets() const;
  bool reconcile_mounts(std::span<const volume::WatchedMount> targets,
                        std::span<const volume::MountObservation> observations);
  [[nodiscard]] std::vector<ContainerOperationTarget> container_operation_targets() override;
  [[nodiscard]] ContainerAStatus container_a_status() override;
  [[nodiscard]] std::vector<OperationEvent> operation_history() override;
  void clear_operation_history() noexcept override;
  void set_creation_feedback(CreationFeedback feedback) override;
  void set_creation_cancellation(CreationCancellation cancelled) override;
  void set_creation_revocation(CreationCancellation revoked) override;
  [[nodiscard]] bool operation_logging_enabled() override;
  bool force_unmount_enabled() override;
  bool hide_new_file_containers() override;
  void set_hide_new_file_containers(bool value) override;
  bool container_file_hidden(std::uint32_t id) override;
  void set_container_file_hidden(std::uint32_t id, bool hidden) override;
  unsigned int container_extension_preference() override;
  void set_container_extension_preference(unsigned int value) override;
  void set_force_unmount_enabled(bool enabled) override;
  bool auto_mount_enabled() override;
  void set_auto_mount_enabled(bool enabled) override;
  void reorder_containers(std::span<const std::uint32_t> ids) override;
  void rename_container(std::uint32_t id, std::wstring_view name) override;
  void set_operation_logging_enabled(bool enabled) override;
  void release_session_secrets() noexcept override;

  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;
  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
