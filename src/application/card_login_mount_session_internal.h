#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/application/mount_orchestrator.h"
#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_directory_listing.h"

namespace usb_vault::application::detail {

class CardLoginMountSessionRuntime {
 public:
  CreationFeedback creation_feedback;
  CreationCancellation creation_cancelled;
  virtual ~CardLoginMountSessionRuntime() noexcept = default;

  [[nodiscard]] virtual MountSessionResult mount_authenticated(
      const ContainerLayout& layout,
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& root_secret) = 0;
  [[nodiscard]] virtual volume::UnmountResult unmount(
      wchar_t drive_letter) = 0;
  [[nodiscard]] virtual volume::UnmountEscalationResult unmount_forced(
      wchar_t drive_letter) = 0;
  [[nodiscard]] virtual bool container_exists(std::wstring_view path) = 0;
  [[nodiscard]] virtual std::optional<std::uint64_t>
  container_size_if_present(std::wstring_view path) = 0;
  // Capacity of the volume the driver currently reports on one drive letter.
  // The backing file cannot answer this question: a hidden volume is smaller
  // than the container hosting it, and even a standard volume is smaller than
  // its file by the header group. No value means the driver has nothing there.
  [[nodiscard]] virtual std::optional<std::uint64_t> mounted_volume_size(
      wchar_t drive_letter) = 0;
  [[nodiscard]] virtual security::SecureBuffer derive_container_password(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& root_secret,
      const security::KdfV1Salt& salt) = 0;
  virtual void create_standard(
      std::wstring_view target_path, std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) = 0;
  // Creates one self-contained outer+hidden container. The helper decides the
  // hidden size from the real allocation bitmap when zero is requested.
  virtual void create_with_hidden(
      std::wstring_view target_path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) = 0;
  virtual void publish_created(std::wstring_view staging_path,
                               std::wstring_view target_path) = 0;
  virtual void remove_incomplete(std::wstring_view staging_path) = 0;
  [[nodiscard]] virtual volume::MountResult mount_created(
      std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) = 0;
  virtual void notify_mounted(wchar_t drive_letter) = 0;
  // Returns a drive letter that the operating system currently reports as
  // free. Letters already recorded by this session are rejected by the caller.
  [[nodiscard]] virtual wchar_t allocate_drive_letter() = 0;
  // The container path index, whole. Reading creates the file at its full size
  // if this machine has never had one, so a first run and a run whose owner
  // has no containers reach the session as the same thing: a file full of
  // slots none of which open.
  [[nodiscard]] virtual std::vector<std::byte> read_container_index() = 0;
  virtual void write_container_index(
      std::span<const std::byte> contents) = 0;
  // Whether the user wants container paths written down at all.
  [[nodiscard]] virtual bool remember_container_paths() = 0;
  virtual void set_remember_container_paths(bool remember) = 0;
  // Whether the user wants an imported container mounted straight away.
  [[nodiscard]] virtual bool mount_imported_containers() = 0;
  virtual void set_mount_imported_containers(bool mount) = 0;
  // The storage layer, reached through here for the same reason as everything
  // else on this interface: the session's own job is turning a container
  // identifier into a checked absolute path, and that is worth testing without
  // a mounted volume underneath it. Every path arriving here is drive-absolute
  // and came out of `resolve_volume_entry_path` or `resolve_volume_child_path`.
  [[nodiscard]] virtual volume::VolumeDirectoryListing list_directory(
      std::wstring_view absolute_directory) = 0;
  virtual void delete_entry(std::wstring_view absolute_path) = 0;
  virtual void create_directory(std::wstring_view absolute_path) = 0;
  // A rename, which is what a move inside one volume is.
  virtual void move_entry(std::wstring_view absolute_source,
                          std::wstring_view absolute_destination) = 0;
  // The move that crosses volumes: every byte read and written, then the
  // source deleted.
  virtual void move_entry_by_copying(
      std::wstring_view absolute_source,
      std::wstring_view absolute_destination) = 0;
  virtual void copy_entry(std::wstring_view absolute_source,
                          std::wstring_view absolute_destination) = 0;
};

// One container adopted by the session. Nothing here is persisted: a restart
// leaves the user reselecting the container files, which is what keeps the
// 8.4 local-state layer free of a container inventory.
struct SessionContainer final {
  std::uint32_t id{};
  std::wstring path;
  std::wstring name;
  wchar_t drive_letter{};
  bool mounted{};
  // Containers with a hidden volume are released before the standard ones,
  // which is the only reason the session distinguishes them. Nothing about
  // this flag crosses the application boundary.
  bool release_first{};
  // Whether the flag above is a fact or a default. A container this session
  // created or opened has said which of its volumes answers; one read back
  // from the index has said nothing, and mounting it has to try both
  // derivations exactly as an import does.
  bool derivation_known{};
};

struct CardLoginMountSessionState final {
  std::vector<SessionContainer> containers;
  std::uint32_t next_container_id{1};
  // Volume password shared by every hidden-volume container, derived on first
  // use from the login input. One derivation opens all of them because they
  // all share a single salt and a single pair of passwords, and the retained
  // value opens nothing else: not the card record, not a standard container,
  // and it cannot be typed at the password prompt.
  security::SecureBuffer hidden_container_password;
  std::array<OperationEvent, ::usb_vault::application::kOperationHistoryCapacity>
      operation_events;
  std::size_t operation_start{};
  std::size_t operation_count{};
  bool operation_logging_enabled{true};
  std::uint64_t next_operation_sequence{1};
  // The password this session was opened under. Unlike the derived value
  // above it is the password itself, kept for the one operation that cannot
  // use a derivative: rewriting the card record so a second password opens it
  // too. Which of the two passwords this is, the session never asks and never
  // records, so retaining it stays symmetric under both logins.
  security::SecureBuffer login_password;

  [[nodiscard]] bool mounted() const noexcept {
    for (const SessionContainer& container : containers) {
      if (container.mounted) return true;
    }
    return false;
  }
};

// Testable core kept private to the application implementation. Friendship on
// CardAuthenticationSession grants only this class scoped access to the root.
class CardLoginMountSessionCore final {
 public:
  CardLoginMountSessionCore(
      CardAuthenticationSession& authentication,
      CardLoginMountSessionRuntime& runtime,
      ContainerLayout layout,
      ::usb_vault::application::LoginMountPolicy login_mount_policy);

  void consume_authenticated_login(
      security::SecureBuffer root_secret,
      security::SecureBuffer login_password_utf8);
  void create_container_a(std::uint64_t size_bytes);
  std::uint32_t create_container(std::wstring_view path,
                                 std::uint64_t size_bytes);
  void mount_adopted_container(std::uint32_t id);
  std::uint32_t create_container_b(
      std::wstring_view path, std::uint64_t outer_size_bytes,
      std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8);
  [[nodiscard]] security::SecureBuffer copy_session_password();
  std::uint32_t mount_container(std::wstring_view path);
  std::uint32_t adopt_container(std::wstring_view path);
  void unmount_container(std::uint32_t id);
  void forget_container(std::uint32_t id);
  [[nodiscard]] bool remembering_container_paths();
  void set_remembering_container_paths(bool remember);
  [[nodiscard]] bool mounting_imported_containers();
  void set_mounting_imported_containers(bool mount);
  [[nodiscard]] volume::VolumeDirectoryListing list_container_directory(
      std::uint32_t id, std::wstring_view relative_directory);
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
  void move_container_entry(std::uint32_t source_id,
                            std::wstring_view source_relative_directory,
                            std::wstring_view name,
                            std::uint32_t destination_id,
                            std::wstring_view destination_relative_directory);
  void copy_container_entry(std::uint32_t source_id,
                            std::wstring_view source_relative_directory,
                            std::wstring_view name,
                            std::uint32_t destination_id,
                            std::wstring_view destination_relative_directory);
  void export_container_entry(
      std::uint32_t id, std::wstring_view relative_directory,
      std::wstring_view name,
      std::wstring_view absolute_destination_directory);
  void export_container_file(
      std::uint32_t id, std::wstring_view absolute_destination_directory);
  [[nodiscard]] std::vector<::usb_vault::application::ContainerStatus>
  container_statuses();
  [[nodiscard]] ::usb_vault::application::ContainerAStatus container_a_status();
  [[nodiscard]] std::vector<::usb_vault::application::OperationEvent>
  operation_history();
  void clear_operation_history() noexcept;
  [[nodiscard]] bool operation_logging_enabled() const noexcept {
    return state_.operation_logging_enabled;
  }
  void set_operation_logging_enabled(bool enabled) noexcept {
    state_.operation_logging_enabled = enabled;
  }
  void release_session_secrets() noexcept;
  void unmount_active();
  [[nodiscard]] bool force_unmount_active();
  [[nodiscard]] bool mounted() const noexcept;

 private:
  [[nodiscard]] security::SecureBuffer derive_standard_password();
  // Derives the shared hidden-volume password on first use and keeps it for
  // the rest of the session. Containers with a hidden volume may live at any
  // path, so login cannot know whether one exists and must not pay for a
  // derivation nothing may need.
  [[nodiscard]] const security::SecureBuffer& hidden_container_password();
  [[nodiscard]] wchar_t reserve_drive_letter();
  // The letter one identifier currently answers on, refusing everything the
  // file manager must not reach: an unauthenticated session, an identifier no
  // container answers to, and a container adopted but not mounted.
  //
  // Authentication is required here as it is for mounting, and unlike for
  // unmounting. Unmounting is how a session gives a volume back and has to
  // stay possible; reading and writing what is inside one is not, and a mount
  // that somehow outlived the card must not keep serving contents.
  [[nodiscard]] wchar_t mounted_drive_letter(std::uint32_t id);
  // Writing a path down, and taking it back out, are conveniences rather than
  // parts of the operations they follow. A container that exists and is
  // mounted has been created whether or not the index could be reached, so
  // neither of these reports a failure and neither can fail an operation.
  [[nodiscard]] security::SecureBuffer container_index_key();
  void remember_container_path(std::wstring_view path) noexcept;
  void forget_container_path(std::wstring_view path) noexcept;
  // Adopts every remembered path whose file is where it was left. Reading is
  // as best-effort as writing: a login that cannot reach the index simply
  // starts with nothing remembered.
  void adopt_remembered_containers() noexcept;
  struct ProbedMount final {
    wchar_t drive_letter{};
    bool hidden_volume{};
  };
  // Mounts a container whose class is not known, standard derivation first,
  // and reports which one opened it along with the letter it took.
  [[nodiscard]] ProbedMount mount_with_either_derivation(
      std::wstring_view path);
  void record_operation(::usb_vault::application::OperationKind kind,
                        ::usb_vault::application::OperationOutcome outcome,
                        std::wstring_view container_name = {},
                        wchar_t drive_letter = wchar_t{}) noexcept;

  CardAuthenticationSession& authentication_;
  CardLoginMountSessionRuntime& runtime_;
  ContainerLayout layout_;
  ::usb_vault::application::LoginMountPolicy login_mount_policy_;
  CardLoginMountSessionState state_;
};

}  // namespace usb_vault::application::detail
