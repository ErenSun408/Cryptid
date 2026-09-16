#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/volume/volume_directory_listing.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {

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

// Whether a successful login also mounts the containers that already exist.
// The product default is `manual`: login establishes authentication only, and
// every mount is a separate, explicitly requested action. `mount_on_login` is
// opt-in and reinstates the P2.14 behaviour of mounting during login.
enum class LoginMountPolicy {
  manual,
  mount_on_login,
};

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

  // Returns a copy of the password this session was opened under. It exists
  // for the single operation that needs the password itself rather than
  // something derived from it: rewriting the card record so a second password
  // opens it too. The session neither knows nor records which of the two
  // passwords this is, so the copy is symmetric under both logins. An
  // unauthenticated session returns an empty buffer.
  [[nodiscard]] virtual security::SecureBuffer copy_session_password() = 0;

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

  // Mounts every adopted container that is not mounted yet, in the fixed
  // B-before-A order Q-07 requires, aborting on the first failure. A session
  // implementation that cannot do this refuses rather than mounting a subset.
  virtual void mount_adopted_all() {
    throw std::logic_error("this session cannot mount every container at once");
  }

  // Unmounts one adopted container. The container stays adopted and unmounted
  // so it can be mounted again without reselecting its file.
  virtual void unmount_container(std::uint32_t id) = 0;
  // Explicit user cleanup retains the authenticated session for remounting.
  // Implementations without this capability fail closed.
  virtual void force_unmount_container(std::uint32_t) {
    throw std::logic_error("manual forced cleanup is unavailable");
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

  // Returns the live, non-secret facts for every container adopted by this
  // session, in adoption order. Nothing is persisted between sessions.
  [[nodiscard]] virtual std::vector<ContainerStatus> container_statuses() = 0;

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
  [[nodiscard]] virtual bool operation_logging_enabled() { return true; }
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
                        std::filesystem::path volume_creator_helper_path,
                        LoginMountPolicy login_mount_policy);
  ~CardLoginMountSession() noexcept override;

  CardLoginMountSession(const CardLoginMountSession&) = delete;
  CardLoginMountSession& operator=(const CardLoginMountSession&) = delete;
  CardLoginMountSession(CardLoginMountSession&&) = delete;
  CardLoginMountSession& operator=(CardLoginMountSession&&) = delete;

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8) override;
  void create_container_a(std::uint64_t size_bytes) override;
  std::uint32_t create_container(std::wstring_view path,
                                 std::uint64_t size_bytes) override;
  std::uint32_t create_container_b(
      std::wstring_view path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8) override;
  [[nodiscard]] security::SecureBuffer copy_session_password() override;
  std::uint32_t mount_container(std::wstring_view path) override;
  void mount_adopted_container(std::uint32_t id) override;
  void mount_adopted_all() override;
  void unmount_container(std::uint32_t id) override;
  void force_unmount_container(std::uint32_t id) override;
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
  [[nodiscard]] std::vector<ContainerStatus> container_statuses() override;
  [[nodiscard]] ContainerAStatus container_a_status() override;
  [[nodiscard]] std::vector<OperationEvent> operation_history() override;
  void clear_operation_history() noexcept override;
  void set_creation_feedback(CreationFeedback feedback) override;
  void set_creation_cancellation(CreationCancellation cancelled) override;
  [[nodiscard]] bool operation_logging_enabled() override;
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
