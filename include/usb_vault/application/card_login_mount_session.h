#pragma once

#include "usb_vault/application/active_volume_session.h"
#include "usb_vault/application/card_login_success_consumer.h"
#include "usb_vault/application/mount_orchestrator.h"

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
  // and the hidden volume with the password the caller is logged in under, so
  // both inputs are required in clear text and are consumed here. Only one
  // such container exists, which keeps the card record out of this operation.
  virtual std::uint32_t create_container_b(
      std::uint64_t outer_size_bytes, std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& decoy_password_utf8) = 0;

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

 protected:
  IAuthenticatedCardVolumeSession() noexcept = default;
};

// P2 Card Record login success handler. It establishes volatile card
// authentication before attempting any post-authentication volume work, never
// reads the card again, and retains only the root plus unified active-volume
// state. The original login input exists only for the synchronous mount call.
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
      const security::SecureBuffer& login_password_utf8,
      const security::SecureBuffer& decoy_password_utf8) override;
  std::uint32_t mount_container(std::wstring_view path) override;
  void mount_adopted_container(std::uint32_t id) override;
  void unmount_container(std::uint32_t id) override;
  [[nodiscard]] std::vector<ContainerStatus> container_statuses() override;
  [[nodiscard]] ContainerAStatus container_a_status() override;

  void unmount_active() override;
  [[nodiscard]] bool force_unmount_active() override;
  [[nodiscard]] bool mounted() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::application
