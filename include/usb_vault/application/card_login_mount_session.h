#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {

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

  // Creates the single outer+hidden container at the fixed layout path and
  // mounts its hidden volume. The outer volume opens with the decoy password
  // supplied here; the hidden volume opens with the password this session was
  // opened under, which the session still holds and the caller no longer has
  // to produce. Only one such container exists, which keeps the card record
  // out of this operation.
  virtual std::uint32_t create_container_b(
      std::uint64_t outer_size_bytes, std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8) = 0;

  // Returns a copy of the password this session was opened under. It exists
  // for the single operation that needs the password itself rather than
  // something derived from it: rewriting the card record so a second password
  // opens it too. The session neither knows nor records which of the two
  // passwords this is, so the copy is symmetric under both logins. An
  // unauthenticated session returns an empty buffer.
  [[nodiscard]] virtual security::SecureBuffer copy_session_password() = 0;

  // Adopts and mounts an existing standard container at the caller-selected
  // path. The session refuses a path it has already adopted.
  virtual std::uint32_t mount_container(std::wstring_view path) = 0;

  // Mounts a container this session already adopted, without asking the user
  // for anything. Standard containers derive from the card alone; the
  // hidden-volume container uses the volume password derived once from the
  // login input, so which of its two volumes appears still follows the
  // password the session was opened with.
  virtual void mount_adopted_container(std::uint32_t id) = 0;

  // Unmounts one adopted container. The container stays adopted and unmounted
  // so it can be mounted again without reselecting its file.
  virtual void unmount_container(std::uint32_t id) = 0;

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
      std::uint64_t outer_size_bytes, std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& decoy_password_utf8) override;
  [[nodiscard]] security::SecureBuffer copy_session_password() override;
  std::uint32_t mount_container(std::wstring_view path) override;
  void mount_adopted_container(std::uint32_t id) override;
  void unmount_container(std::uint32_t id) override;
  [[nodiscard]] std::vector<ContainerStatus> container_statuses() override;
  [[nodiscard]] ContainerAStatus container_a_status() override;
  [[nodiscard]] std::vector<OperationEvent> operation_history() override;
  void clear_operation_history() noexcept override;
  void release_session_secrets() noexcept override;

  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;
  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
