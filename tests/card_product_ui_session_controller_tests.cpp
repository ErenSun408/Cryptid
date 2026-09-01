#include "usb_vault/ui/card_product_ui_session_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/offline_card_password_policy.h"

#include <array>
#include <concepts>
#include <cstddef>
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

  std::uint32_t create_container_b(std::uint64_t, std::uint64_t,
                                   const SecureBuffer&,
                                   const SecureBuffer&) override {
    throw std::logic_error("container B is not part of this slice");
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
           .size_bytes = entry.size_bytes,
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

  void consume_authenticated_login(
      SecureBuffer root,
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
  std::wstring created_path;
  std::wstring mounted_path;
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  // Lets one test hand the controller an unmounted container that still
  // reports a drive letter, which must fail closed instead of being shown.
  wchar_t inconsistent_unmounted_drive{};
  bool reject_create{};
  bool reject_mount{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};
  int container_statuses_calls{};
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

  void set_second_card_password(SecureBuffer, SecureBuffer) override {
    throw std::logic_error("card record updates are not part of this slice");
  }

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }

  std::optional<CardEntryRoute> route;
  std::vector<std::byte> initialization_bytes;
  bool reject_credential{};
  bool fail_system{};
  int initialization_calls{};
  int login_calls{};
};

// The page never supplies a path, so every selection in these tests comes
// from the chooser and the counters prove when it was consulted.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  [[nodiscard]] std::wstring choose_new_container() override {
    ++new_calls;
    return new_path;
  }

  [[nodiscard]] std::wstring choose_existing_container() override {
    ++existing_calls;
    return existing_path;
  }

  std::wstring new_path;
  std::wstring existing_path;
  int new_calls{};
  int existing_calls{};
};

struct Fixture final {
  Fixture()
      : volume(authentication),
        controller(entry, authentication, volume, policy, chooser) {}

  FakeEntry entry;
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
  fixture.chooser.new_path = L"D:\\Vault\\work.hc";
  require(fixture.controller.create_container(64),
          "creation reported a cancellation the chooser did not make");

  std::vector<ContainerUiStatus> statuses =
      fixture.controller.container_statuses();
  require(statuses.size() == 1 && statuses[0].id != 0 &&
              statuses[0].name == L"work.hc" && statuses[0].mounted &&
              statuses[0].size_mib == 64 && statuses[0].drive_letter == L'D',
          "container projection lost identity, name, capacity, or drive");
  require(fixture.volume.created_path == L"D:\\Vault\\work.hc" &&
              fixture.volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL,
          "the chosen path or capacity did not reach the session");

  fixture.controller.unmount_container(statuses[0].id);
  statuses = fixture.controller.container_statuses();
  require(statuses.size() == 1 && !statuses[0].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              statuses[0].size_mib == 64,
          "an unmounted container was dropped or kept a drive letter");
}

void test_container_creation_is_bounded_and_cancellable() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  require_throws<std::system_error>(
      [&] { static_cast<void>(fixture.controller.create_container(64)); },
      "unauthenticated route created a container");

  fixture.authentication.establish(root_secret());
  for (const std::uint32_t invalid : {kContainerCreationMinimumMib - 1,
                                      kContainerCreationMaximumMib + 1}) {
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(fixture.controller.create_container(invalid));
        },
        "UI accepted a container size outside the shared range");
  }
  require(fixture.chooser.new_calls == 0 && fixture.volume.create_calls == 0,
          "an out-of-range size still opened the chooser or the session");

  fixture.chooser.new_path = L"D:\\Vault\\work.hc";
  fixture.volume.reject_create = true;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.create_container(64)); },
      "a failed creation was not reported");
  require(fixture.controller.state() == CardProductUiState::authenticated,
          "a failed creation dropped the authenticated route");
  fixture.volume.reject_create = false;

  fixture.chooser.new_path.clear();
  require(!fixture.controller.create_container(64) &&
              fixture.chooser.new_calls == 2 &&
              fixture.volume.create_calls == 1,
          "a cancelled chooser still created a container");
  require(fixture.controller.container_statuses().empty(),
          "a cancelled creation left an adopted container behind");
}

void test_adopted_container_remounts_without_reselecting_its_file() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.chooser.new_path = L"D:\\Vault\\work.hc";
  require(fixture.controller.create_container(64), "creation was refused");
  const std::uint32_t id = fixture.controller.container_statuses()[0].id;

  fixture.controller.unmount_container(id);
  require(fixture.controller.mount_container(id) &&
              fixture.chooser.existing_calls == 0 &&
              fixture.volume.mounted_path == L"D:\\Vault\\work.hc",
          "remounting an adopted container asked for the file again");
  require(fixture.controller.container_statuses()[0].mounted,
          "the remounted container is not reported as mounted");

  require(fixture.controller.mount_container(id) &&
              fixture.volume.mount_calls == 1,
          "remounting a mounted container repeated the session call");
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(fixture.controller.mount_container(id + 7)); },
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

void test_inconsistent_container_status_fails_closed() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.chooser.new_path = L"D:\\Vault\\work.hc";
  require(fixture.controller.create_container(64), "creation was refused");
  const std::uint32_t id = fixture.controller.container_statuses()[0].id;
  fixture.controller.unmount_container(id);

  fixture.volume.inconsistent_unmounted_drive = L'D';
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(fixture.controller.container_statuses()); },
      "UI displayed a drive letter for an unmounted container");
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
    test_container_creation_is_bounded_and_cancellable();
    test_adopted_container_remounts_without_reselecting_its_file();
    test_inconsistent_container_status_fails_closed();
    std::cout << "card_product_ui_session_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_ui_session_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
