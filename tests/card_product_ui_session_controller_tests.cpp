#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/offline_card_password_policy.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardAuthenticationSession;
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
           .size_bytes = entry.mounted ? entry.size_bytes : 0,
           .drive_letter = entry.mounted ? entry.drive_letter
                                         : inconsistent_unmounted_drive});
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
  std::wstring created_path;
  std::wstring mounted_path;
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  // Lets one test hand the controller an unmounted container that still
  // reports a drive letter, which must fail closed instead of being shown.
  wchar_t inconsistent_unmounted_drive{};
  bool reject_create{};
  bool reject_create_b{};
  bool reject_mount{};
  std::wstring created_b_path;
  std::uint64_t created_outer_bytes{};
  std::uint64_t created_hidden_bytes{};
  SecureBuffer copy_session_password() override {
    ++session_password_calls;
    SecureBuffer copy(session_password.size());
    for (std::size_t index = 0; index < session_password.size(); ++index) {
      copy.bytes()[index] = session_password[index];
    }
    return copy;
  }

  std::vector<std::byte> b_decoy_password;
  std::vector<std::byte> session_password{bytes("login-password")};
  int session_password_calls{};
  int create_b_calls{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};
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

  int reset_calls{};
  bool reject_reset{};

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }

  std::optional<CardEntryRoute> route;
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

  // The path the controller must produce for a container named `name`.
  [[nodiscard]] std::wstring target(const std::wstring_view name) const {
    return wide() + L"\\" + std::wstring(name) + L".hc";
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
  require_wiped(password, "initialization password was not wiped");
  require_wiped(confirmation, "initialization confirmation was not wiped");
  require(equals(fixture.entry.initialization_bytes, expected) &&
              fixture.entry.initialization_calls == 1 &&
              fixture.controller.state() == CardProductUiState::authenticated,
          "initialization did not normalize, prove, or authenticate");
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
              statuses[0].name == L"work.hc" && statuses[0].mounted &&
              statuses[0].size_mib == 64 && statuses[0].drive_letter == L'D',
          "container projection lost identity, name, capacity, or drive");
  // The name is the file name: the extension belongs to this boundary, not to
  // the page, and the directory is joined here rather than on the page.
  require(fixture.volume.created_path == fixture.directory.target(L"work") &&
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
              fixture.volume.created_b_path ==
                  fixture.directory.target(L"handover"),
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
  fixture.controller.create_container_b(L"spare", directory, 40, 20,
                                        again_second, again_confirmation);
  require(fixture.volume.create_b_calls == 2 &&
              fixture.volume.created_b_path ==
                  fixture.directory.target(L"spare") &&
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

}  // namespace

int main() {
  try {
    test_state_projection_is_complete_and_authentication_wins();
    test_login_normalizes_wipes_and_preserves_retry_semantics();
    test_post_authentication_failure_projects_authenticated_state();
    test_initialization_normalizes_both_inputs_and_creates_proof();
    test_initialization_rejections_wipe_both_before_entry_action();
    test_wrong_route_wipes_without_secret_processing();
    test_overlapping_initialization_inputs_are_rejected_and_wiped();
    test_residual_mount_blocks_new_card_actions_until_cleanup();
    test_unmount_forwards_only_unified_active_state();
    test_container_collection_is_projected_only_when_authenticated();
    test_container_creation_is_bounded_and_validated();
    test_browsing_for_a_directory_only_reports_the_choice();
    test_adopted_container_remounts_without_reselecting_its_file();
    test_container_b_commits_the_card_before_the_volume();
    test_container_b_rejects_unusable_requests();
    test_capacity_rounds_the_attached_volume_to_whole_mib();
    test_card_reset_releases_the_session_and_refuses_while_mounted();
    test_inconsistent_container_status_fails_closed();
    test_operation_history_is_authenticated_sanitized_and_clearable();
    test_operation_history_rejects_paths_and_impossible_shapes();
    std::cout << "card_product_ui_session_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_ui_session_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
