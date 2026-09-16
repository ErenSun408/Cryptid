#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/container_file_chooser.h"

#include <cstddef>
#include <cstdint>
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
      IContainerFileChooser& file_chooser) noexcept;

  void submit_login(std::span<std::byte> mutable_utf8);
  void submit_initialization(
      std::span<std::byte> mutable_password_utf8,
      std::span<std::byte> mutable_confirmation_utf8);

  // Opens the native folder picker behind the form's browse button and returns
  // the directory the user chose, or an empty string when they cancel. It only
  // fills a field: what comes back is checked on the way in again, exactly as
  // a typed directory is, so nothing here is trusted for having been picked.
  [[nodiscard]] std::wstring browse_for_container_directory();

  // Creates one standard container of the requested size, named `name`, inside
  // `directory`. The two are joined and checked by
  // `resolve_container_target_path`; a rejected name or location, a directory
  // that does not exist, an out-of-range size, or a wrong route all throw
  // `std::invalid_argument`. There is no cancel outcome any more: the user
  // settles every field before the create button commits.
  void create_container(std::wstring_view name, std::wstring_view directory,
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
  void create_container_b(
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

  void unmount_container(std::uint32_t id);
  void force_unmount_container(std::uint32_t id);
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
  [[nodiscard]] std::vector<OperationUiEvent> operation_history();
  void clear_operation_history();
  void set_creation_feedback(application::CreationFeedback feedback);
  void set_creation_cancellation(application::CreationCancellation cancelled);
  [[nodiscard]] bool operation_logging_enabled();
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

 private:
  application::ICardEntryActions& entry_;
  application::CardAuthenticationSession& authentication_;
  application::IAuthenticatedCardVolumeSession& volume_session_;
  const security::IPasswordStrengthPolicy& password_policy_;
  IContainerFileChooser& file_chooser_;
};

}  // namespace usb_vault::ui
