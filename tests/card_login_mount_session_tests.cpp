#include "application/card_login_mount_session_internal.h"

#include "usb_vault/application/application_session.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/card/card_record_v1.h"

#include <cstddef>
#include <concepts>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardLoginMountSession;
using usb_vault::application::ContainerLayout;
using usb_vault::application::LoginMountPolicy;
using usb_vault::application::ContainerCreationCleanupError;
using usb_vault::application::MountRollbackError;
using usb_vault::application::MountSessionResult;
using usb_vault::application::detail::CardLoginMountSessionCore;
using usb_vault::application::detail::CardLoginMountSessionRuntime;
using usb_vault::security::SecureBuffer;

static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::ICardLoginSuccessConsumer>);
static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::IActiveVolumeSession>);
static_assert(!std::derived_from<
              CardLoginMountSession,
              usb_vault::application::IApplicationSessionActions>);

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer bytes(const std::string_view value) {
  SecureBuffer result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(value[index]);
  }
  return result;
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
  return result;
}

class FakeRuntime final : public CardLoginMountSessionRuntime {
 public:
  bool mount_a{};
  bool mount_b{};
  bool fail_mount{};
  bool leave_b_after_failure{};
  bool fail_unmount_a{};
  bool fail_unmount_b{};
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
  std::vector<std::string> events;

  [[nodiscard]] MountSessionResult mount_authenticated(
      const ContainerLayout& received_layout,
      const SecureBuffer& login_password_utf8,
      const SecureBuffer& root) override {
    events.emplace_back("mount");
    require(received_layout.container_a.drive_letter == L'X' &&
                received_layout.container_b.drive_letter == L'Y',
            "mount received the wrong layout");
    require(login_password_utf8.size() == 14,
            "mount received the wrong login input");
    require(root.size() == usb_vault::card::kCardRecordV1RootSecretSize,
            "mount received the wrong authenticated root");

    if (leave_b_after_failure) {
      throw MountRollbackError(
          L'Y', std::make_exception_ptr(std::runtime_error("A failed")),
          std::make_exception_ptr(std::runtime_error("rollback failed")));
    }
    if (fail_mount) throw std::runtime_error("mount failed");

    MountSessionResult result;
    if (mount_a) result.container_a.emplace();
    if (mount_b) result.container_b.emplace();
    return result;
  }

  [[nodiscard]] usb_vault::volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
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
    throw std::runtime_error("unexpected drive letter");
  }

  [[nodiscard]] usb_vault::volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    if (drive_letter == L'Y') {
      events.emplace_back("force:B");
      return {.result = {}, .forced = force_required_b};
    }
    if (drive_letter == L'X') {
      events.emplace_back("force:A");
      return {.result = {}, .forced = force_required_a};
    }
    throw std::runtime_error("unexpected drive letter");
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
    throw std::runtime_error("unexpected creation path");
  }

  [[nodiscard]] std::optional<std::uint64_t> container_size_if_present(
      const std::wstring_view path) override {
    events.emplace_back("size:A");
    require(path == L"C:\\vault\\a.hc",
            "status inspected the wrong container path");
    return container_a_exists
               ? std::optional<std::uint64_t>{container_a_size_bytes}
               : std::nullopt;
  }

  [[nodiscard]] SecureBuffer derive_container_password(
      const SecureBuffer& password_input,
      const SecureBuffer& root,
      const usb_vault::security::KdfV1Salt& salt) override {
    events.emplace_back("derive:A");
    require(password_input.size() == 5 &&
                password_input.bytes()[0] == std::byte{'d'} &&
                password_input.bytes()[1] == std::byte{'i'} &&
                password_input.bytes()[2] == std::byte{'s'} &&
                password_input.bytes()[3] == std::byte{'k'} &&
                password_input.bytes()[4] == std::byte{'A'},
            "creation did not use the fixed diskA input");
    require(root.size() == usb_vault::card::kCardRecordV1RootSecretSize,
            "creation received the wrong authenticated root");
    require(salt == layout().container_a.salt,
            "creation received the wrong A salt");
    return bytes("derived-container-a-password");
  }

  void create_standard(
      const std::wstring_view target_path,
      const std::uint64_t size_bytes,
      const SecureBuffer& volume_password) override {
    events.emplace_back("create:A");
    require(target_path == L"C:\\vault\\a.hc.preparing",
            "creation did not use the staging path");
    require(size_bytes == 64ULL * 1024ULL * 1024ULL,
            "creation received the wrong size");
    require(volume_password.size() == 28,
            "creation received the wrong derived password");
    if (fail_create) throw std::runtime_error("create failed");
    container_a_size_bytes = size_bytes;
    staging_exists = true;
  }

  void publish_created(const std::wstring_view staging_path,
                       const std::wstring_view target_path) override {
    events.emplace_back("publish:A");
    require(staging_path == L"C:\\vault\\a.hc.preparing" &&
                target_path == L"C:\\vault\\a.hc",
            "creation published the wrong paths");
    if (fail_publish) throw std::runtime_error("publish failed");
    staging_exists = false;
    container_a_exists = true;
  }

  void remove_incomplete(const std::wstring_view staging_path) override {
    events.emplace_back("cleanup:A");
    require(staging_path == L"C:\\vault\\a.hc.preparing",
            "creation cleaned the wrong path");
    if (fail_remove) throw std::runtime_error("cleanup failed");
    staging_exists = false;
  }

  [[nodiscard]] usb_vault::volume::MountResult mount_created(
      const std::wstring_view path,
      const SecureBuffer& volume_password,
      const usb_vault::volume::MountOptions& options) override {
    events.emplace_back("mount-created:A");
    require(path == L"C:\\vault\\a.hc" && container_a_exists,
            "creation mounted before publication");
    require(volume_password.size() == 28,
            "created mount received the wrong derived password");
    require(options.drive_letter == L'X' &&
                options.prf == usb_vault::volume::Pkcs5Prf::sha512 &&
                options.pim == 0 && !options.read_only &&
                !options.notify_shell,
            "created mount received the wrong fixed options");
    if (fail_created_mount) throw std::runtime_error("mount created failed");
    return {};
  }

  void notify_mounted(const wchar_t drive_letter) override {
    events.emplace_back("notify:A");
    require(drive_letter == L'X', "creation notified the wrong drive");
    if (fail_notify) throw std::runtime_error("notify failed");
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

void require_events(const FakeRuntime& runtime,
                    const std::initializer_list<std::string_view> expected) {
  require(runtime.events.size() == expected.size(),
          "runtime event count differed");
  std::size_t index = 0;
  for (const std::string_view event : expected) {
    require(runtime.events[index] == event, "runtime event order differed");
    ++index;
  }
}

void test_all_container_shapes_establish_authentication() {
  for (const auto [mount_a, mount_b] : {
           std::pair{false, false}, std::pair{true, false},
           std::pair{false, true}, std::pair{true, true}}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    runtime.mount_a = mount_a;
    runtime.mount_b = mount_b;
    CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
    SecureBuffer root = root_secret();
    SecureBuffer password = bytes("login-password");

    session.consume_authenticated_login(std::move(root),
                                        std::move(password));

    require(root.empty() && password.empty(),
            "successful handoff copied an input owner");
    require(authentication.authenticated(),
            "successful card login did not establish authentication");
    require(session.mounted() == (mount_a || mount_b),
            "container shape produced the wrong unified mount state");
    require_events(runtime, {"mount"});
  }
}

void test_mount_failures_do_not_revoke_card_authentication() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.fail_mount = true;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);

  require_throws<std::runtime_error>(
      [&] {
        session.consume_authenticated_login(root_secret(),
                                            bytes("login-password"));
      },
      "ordinary mount failure was not propagated");
  require(authentication.authenticated() && !session.mounted(),
          "post-authentication mount failure revoked authentication or "
          "invented mount state");
  require_events(runtime, {"mount"});
}

void test_failed_rollback_retains_only_residual_mount() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.leave_b_after_failure = true;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);

  require_throws<MountRollbackError>(
      [&] {
        session.consume_authenticated_login(root_secret(),
                                            bytes("login-password"));
      },
      "residual rollback failure was not propagated");
  require(authentication.authenticated() && session.mounted(),
          "residual mount or authenticated state was lost");

  runtime.leave_b_after_failure = false;
  session.unmount_active();
  require(!session.mounted(), "residual mount could not be retried");
  require_events(runtime, {"mount", "unmount:B"});
}

void test_invalid_or_duplicate_handoff_fails_before_mount() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);

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

void test_unmount_state_is_precise_and_forced_order_is_b_then_a() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.mount_a = true;
  runtime.mount_b = true;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  runtime.events.clear();

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
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);

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

void test_container_a_creation_publishes_then_mounts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
  establish_empty_authenticated_session(authentication, runtime, session);

  session.create_container_a(64ULL * 1024ULL * 1024ULL);

  require(authentication.authenticated() && session.mounted(),
          "successful A creation lost authentication or mount state");
  require(runtime.container_a_exists && !runtime.staging_exists,
          "successful A creation left the wrong published state");
  require_events(runtime,
                 {"exists:final", "exists:stage", "derive:A", "create:A",
                  "publish:A", "mount-created:A", "notify:A"});

  require_throws<std::logic_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "mounted A accepted duplicate creation");
}

void test_container_a_creation_rejects_existing_targets() {
  for (const bool final_exists : {true, false}) {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
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
    CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
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
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
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

void test_container_a_mount_failure_keeps_published_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
  establish_empty_authenticated_session(authentication, runtime, session);
  runtime.fail_created_mount = true;

  require_throws<std::runtime_error>(
      [&] { session.create_container_a(64ULL * 1024ULL * 1024ULL); },
      "post-publication mount failure was hidden");
  require(runtime.container_a_exists && !runtime.staging_exists &&
              !session.mounted() && authentication.authenticated(),
          "post-publication mount failure deleted data or changed auth");
  require_events(runtime,
                 {"exists:final", "exists:stage", "derive:A", "create:A",
                  "publish:A", "mount-created:A"});
}

void test_container_a_notify_failure_retains_mount_state() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
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

void test_container_a_status_projects_live_file_and_mount_facts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
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
  mounted_runtime.mount_a = true;
  mounted_runtime.container_a_exists = true;
  CardLoginMountSessionCore mounted_session(
      mounted_authentication, mounted_runtime, layout(),
      LoginMountPolicy::mount_on_login);
  mounted_session.consume_authenticated_login(root_secret(),
                                               bytes("login-password"));
  mounted_runtime.events.clear();
  const auto mounted = mounted_session.container_a_status();
  require(mounted.exists && mounted.mounted &&
              mounted.size_bytes == 64ULL * 1024ULL * 1024ULL &&
              mounted.drive_letter == L'X',
          "mounted A status did not project its fixed drive and capacity");

  mounted_runtime.container_a_exists = false;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(mounted_session.container_a_status()); },
      "missing backing file was hidden for a recorded A mount");
}

void test_manual_policy_authenticates_without_mounting() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.mount_a = true;
  runtime.mount_b = true;
  runtime.container_a_exists = true;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);

  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  require(authentication.authenticated(),
          "the manual policy did not establish card authentication");
  require(runtime.events.empty(),
          "the manual policy reached the mount runtime during login");
  require(!session.mounted(),
          "the manual policy reported an active volume after login");

  const auto status = session.container_a_status();
  require(status.exists && !status.mounted && status.drive_letter == wchar_t{},
          "an existing container was projected as mounted after manual login");
}

void test_manual_policy_still_serializes_and_allows_creation() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
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

}  // namespace

int main() {
  try {
    test_all_container_shapes_establish_authentication();
    test_mount_failures_do_not_revoke_card_authentication();
    test_failed_rollback_retains_only_residual_mount();
    test_invalid_or_duplicate_handoff_fails_before_mount();
    test_unmount_state_is_precise_and_forced_order_is_b_then_a();
    test_container_a_creation_requires_authentication_and_valid_size();
    test_container_a_creation_publishes_then_mounts();
    test_container_a_creation_rejects_existing_targets();
    test_container_a_creation_cleans_prepublication_failures();
    test_container_a_creation_reports_cleanup_failure();
    test_container_a_mount_failure_keeps_published_container();
    test_container_a_notify_failure_retains_mount_state();
    test_container_a_status_projects_live_file_and_mount_facts();
    test_manual_policy_authenticates_without_mounting();
    test_manual_policy_still_serializes_and_allows_creation();
    std::cout << "card_login_mount_session_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_login_mount_session_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
