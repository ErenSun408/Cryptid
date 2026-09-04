#include "application/card_login_mount_session_internal.h"

#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/card/card_record_v1.h"

#include <cstddef>
#include <concepts>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardLoginMountSession;
using usb_vault::application::ContainerLayout;
using usb_vault::application::ContainerStatus;
using usb_vault::application::LoginMountPolicy;
using usb_vault::application::detail::SessionContainer;
using usb_vault::application::ContainerCreationCleanupError;
using usb_vault::application::MountRollbackError;
using usb_vault::application::MountSessionResult;
using usb_vault::application::OperationEvent;
using usb_vault::application::OperationKind;
using usb_vault::application::OperationOutcome;
using usb_vault::application::detail::CardLoginMountSessionCore;
using usb_vault::application::detail::CardLoginMountSessionRuntime;
using usb_vault::security::SecureBuffer;

static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::ICardLoginSuccessConsumer>);
static_assert(std::derived_from<
              CardLoginMountSession,
              usb_vault::application::IActiveVolumeSession>);

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::vector<std::byte> bytes_vector(
    const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
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
  // The two containers derive under different salts, exactly as the
  // production layout does; otherwise this fixture cannot tell them apart.
  result.container_b.salt.fill(std::byte{0xB0});
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
  std::vector<wchar_t> free_drive_letters{L'X', L'W', L'V'};
  bool fail_allocate{};
  std::uint64_t other_size_bytes{};
  std::uint64_t outer_size{};
  std::uint64_t hidden_size{};
  std::vector<std::byte> hidden_outer_password;
  std::vector<std::byte> hidden_inner_password;
  std::vector<std::byte> last_mount_password;
  std::vector<std::wstring> other_existing;
  std::vector<std::wstring> other_staging;
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
    events.emplace_back("unmount:other");
    return {};
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
    events.emplace_back("force:other");
    return {.result = {}, .forced = false};
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
    const bool staging = path.ends_with(L".preparing");
    events.emplace_back(staging ? "exists:stage-other" : "exists:final-other");
    const std::vector<std::wstring>& known =
        staging ? other_staging : other_existing;
    for (const std::wstring& present : known) {
      if (present == path) return true;
    }
    return false;
  }

  [[nodiscard]] std::optional<std::uint64_t> container_size_if_present(
      const std::wstring_view path) override {
    if (path == L"C:\\vault\\a.hc") {
      events.emplace_back("size:A");
      return container_a_exists
                 ? std::optional<std::uint64_t>{container_a_size_bytes}
                 : std::nullopt;
    }
    events.emplace_back("size:other");
    for (const std::wstring& present : other_existing) {
      if (present == path) {
        return std::optional<std::uint64_t>{other_size_bytes};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] SecureBuffer derive_container_password(
      const SecureBuffer& password_input,
      const SecureBuffer& root,
      const usb_vault::security::KdfV1Salt& salt) override {
    require(root.size() == usb_vault::card::kCardRecordV1RootSecretSize,
            "creation received the wrong authenticated root");
    if (salt == layout().container_b.salt) {
      events.emplace_back("derive:B");
      std::string material = "derived-b-";
      for (const std::byte value : password_input.bytes()) {
        material.push_back(static_cast<char>(value));
      }
      material.resize(28, '.');
      return bytes(material);
    }
    events.emplace_back("derive:A");
    require(password_input.size() == 5 &&
                password_input.bytes()[0] == std::byte{'d'} &&
                password_input.bytes()[1] == std::byte{'i'} &&
                password_input.bytes()[2] == std::byte{'s'} &&
                password_input.bytes()[3] == std::byte{'k'} &&
                password_input.bytes()[4] == std::byte{'A'},
            "creation did not use the fixed diskA input");
    require(salt == layout().container_a.salt,
            "every standard container must share the fixed A salt");
    return bytes("derived-container-a-password");
  }

  void create_standard(
      const std::wstring_view target_path,
      const std::uint64_t size_bytes,
      const SecureBuffer& volume_password) override {
    const bool is_container_a = target_path == L"C:\\vault\\a.hc.preparing";
    events.emplace_back(is_container_a ? "create:A" : "create:other");
    require(target_path.ends_with(L".preparing"),
            "creation did not use the staging path");
    require(size_bytes != 0 && size_bytes % 512ULL == 0,
            "creation received the wrong size");
    require(volume_password.size() == 28,
            "creation received the wrong derived password");
    if (fail_create) throw std::runtime_error("create failed");
    if (is_container_a) {
      container_a_size_bytes = size_bytes;
      staging_exists = true;
    } else {
      other_size_bytes = size_bytes;
      other_staging.emplace_back(target_path);
    }
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer& outer_password,
      const SecureBuffer& hidden_password) override {
    events.emplace_back("create:hidden");
    require(target_path.ends_with(L".preparing"),
            "hidden creation did not use a staging path");
    hidden_target_paths.emplace_back(target_path);
    require(outer_size_bytes != 0 && outer_size_bytes % 512ULL == 0 &&
                hidden_size_bytes % 512ULL == 0 &&
                hidden_size_bytes < outer_size_bytes,
            "hidden creation received unusable sizes");
    require(outer_password.size() == 28 && hidden_password.size() == 28,
            "hidden creation received the wrong derived passwords");
    outer_size = outer_size_bytes;
    hidden_size = hidden_size_bytes;
    hidden_outer_password.assign(outer_password.bytes().begin(),
                                 outer_password.bytes().end());
    hidden_inner_password.assign(hidden_password.bytes().begin(),
                                 hidden_password.bytes().end());
    require(hidden_outer_password != hidden_inner_password,
            "both volumes were created with the same password");
    if (fail_create) throw std::runtime_error("create failed");
    other_staging.emplace_back(target_path);
  }

  void publish_created(const std::wstring_view staging_path,
                       const std::wstring_view target_path) override {
    const bool is_container_a = target_path == L"C:\\vault\\a.hc";
    events.emplace_back(is_container_a ? "publish:A" : "publish:other");
    require(staging_path == std::wstring(target_path) + L".preparing",
            "creation published the wrong paths");
    if (fail_publish) throw std::runtime_error("publish failed");
    if (is_container_a) {
      staging_exists = false;
      container_a_exists = true;
    } else {
      std::erase(other_staging, std::wstring(staging_path));
      other_existing.emplace_back(target_path);
    }
  }

  void remove_incomplete(const std::wstring_view staging_path) override {
    events.emplace_back(staging_path == L"C:\\vault\\a.hc.preparing" ? "cleanup:A" : "cleanup:other");
    require(staging_path.ends_with(L".preparing"),
            "creation cleaned the wrong path");
    if (fail_remove) throw std::runtime_error("cleanup failed");
    staging_exists = false;
    std::erase(other_staging, std::wstring(staging_path));
  }

  [[nodiscard]] usb_vault::volume::MountResult mount_created(
      const std::wstring_view path,
      const SecureBuffer& volume_password,
      const usb_vault::volume::MountOptions& options) override {
    const bool is_container_a = path == L"C:\\vault\\a.hc";
    events.emplace_back(is_container_a ? "mount-created:A"
                                       : "mount-created:other");
    require(!is_container_a || container_a_exists,
            "creation mounted before publication");
    require(volume_password.size() == 28,
            "created mount received the wrong derived password");
    require(options.drive_letter == allocated_drive_letter &&
                options.prf == usb_vault::volume::Pkcs5Prf::sha512 &&
                options.pim == 0 && !options.read_only &&
                !options.notify_shell,
            "created mount received the wrong fixed options");
    last_mount_password.assign(volume_password.bytes().begin(),
                               volume_password.bytes().end());
    if (!login_derived_path.empty() && path == login_derived_path &&
        (nothing_opens_it || !derived_from_login(volume_password))) {
      // Exactly what the driver reports for a header that no candidate
      // password unlocks, which is the only signal the session gets before it
      // tries the other derivation.
      throw std::system_error(
          usb_vault::volume::make_error_code(
              usb_vault::volume::DriverError::authentication_failed),
          "wrong password");
    }
    if (fail_created_mount) throw std::runtime_error("mount created failed");
    return {};
  }

  void notify_mounted(const wchar_t drive_letter) override {
    events.emplace_back("notify:A");
    require(drive_letter == allocated_drive_letter,
            "creation notified the wrong drive");
    if (fail_notify) throw std::runtime_error("notify failed");
  }

  [[nodiscard]] wchar_t allocate_drive_letter() override {
    events.emplace_back("allocate");
    if (fail_allocate) throw std::runtime_error("no free drive letter");
    require(!free_drive_letters.empty(), "the fake ran out of drive letters");
    allocated_drive_letter = free_drive_letters.front();
    free_drive_letters.erase(free_drive_letters.begin());
    return allocated_drive_letter;
  }

  wchar_t allocated_drive_letter{L'X'};
  // A container whose volumes were built from the login input. Only the B
  // derivation opens it, exactly as on a real hidden-volume container. Set
  // `nothing_opens_it` alongside it for a file that is not one of ours.
  std::wstring login_derived_path;
  bool nothing_opens_it{};
  std::vector<std::wstring> hidden_target_paths;

 private:
  [[nodiscard]] static bool derived_from_login(
      const SecureBuffer& volume_password) noexcept {
    constexpr std::string_view prefix = "derived-b-";
    if (volume_password.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (volume_password.bytes()[index] !=
          static_cast<std::byte>(prefix[index])) {
        return false;
      }
    }
    return true;
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
  bool matched = runtime.events.size() == expected.size();
  std::size_t index = 0;
  for (const std::string_view event : expected) {
    if (!matched) break;
    matched = runtime.events[index] == event;
    ++index;
  }
  if (!matched) {
    std::string observed;
    for (const std::string& event : runtime.events) {
      if (!observed.empty()) observed += ", ";
      observed += event;
    }
    throw std::runtime_error("runtime event sequence differed; observed: " +
                             observed);
  }
}

void require_operation(const OperationEvent& event,
                       const std::uint64_t sequence,
                       const OperationKind kind,
                       const OperationOutcome outcome,
                       const std::wstring_view container_name,
                       const wchar_t drive_letter,
                       const char* const message) {
  require(
      event.sequence == sequence && event.completed_at_unix_ms > 0 &&
          event.kind == kind && event.outcome == outcome &&
          event.container_name == container_name &&
          event.container_name.find_first_of(L"\\/") == std::wstring::npos &&
          event.drive_letter == drive_letter,
      message);
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
    require_events(runtime, {"exists:final-other", "exists:final", "mount"});
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
  require_events(runtime, {"exists:final-other", "exists:final", "mount"});
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
  require_events(runtime, {"exists:final-other", "exists:final", "mount", "unmount:B"});
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
                  "publish:A", "allocate", "mount-created:A", "notify:A"});

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
                  "publish:A", "allocate", "mount-created:A"});
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

[[nodiscard]] FakeRuntime runtime_with_both_containers() {
  FakeRuntime runtime;
  runtime.container_a_exists = true;
  runtime.container_a_size_bytes = 64ULL * 1024ULL * 1024ULL;
  runtime.other_existing.emplace_back(L"C:\\vault\\b.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  return runtime;
}

void test_manual_login_adopts_without_deriving_anything() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);

  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));

  // Containers with a hidden volume live wherever the user put them, so login
  // cannot tell whether one exists and must not pay for a derivation that may
  // open nothing. Both derivations wait for the mount that needs them.
  require_events(runtime, {"exists:final-other", "exists:final"});
  require(!session.mounted(), "manual login mounted a container");
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].name == L"b.hc" &&
              statuses[1].name == L"a.hc" && !statuses[0].mounted &&
              !statuses[1].mounted,
          "manual login did not adopt both containers unmounted");
}

void test_adopted_containers_mount_without_further_input() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::vector<ContainerStatus> adopted = session.container_statuses();
  const std::uint32_t hidden_id = adopted[0].id;
  const std::uint32_t standard_id = adopted[1].id;

  runtime.events.clear();
  session.mount_adopted_container(hidden_id);
  require(runtime.last_mount_password ==
              bytes_vector("derived-b-login-password...."),
          "the hidden container was not opened with the captured password");
  require_events(runtime, {"exists:final-other", "derive:B", "allocate",
                           "mount-created:other", "notify:A"});

  runtime.events.clear();
  session.mount_adopted_container(standard_id);
  require(runtime.last_mount_password ==
              bytes_vector("derived-container-a-password"),
          "the standard container was not opened with the card derivation");
  require_events(runtime, {"exists:final", "derive:A", "allocate",
                           "mount-created:A", "notify:A"});

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].mounted && statuses[1].mounted &&
              statuses[0].drive_letter != statuses[1].drive_letter,
          "the adopted containers did not both mount on distinct drives");

  // Mounting an already mounted container is a no-op, and an unknown
  // identifier is refused.
  runtime.events.clear();
  session.mount_adopted_container(hidden_id);
  require(runtime.events.empty(), "a mounted container was mounted again");
  require_throws<std::invalid_argument>(
      [&] { session.mount_adopted_container(hidden_id + 100); },
      "an unknown identifier was accepted");
}

void test_forced_release_drops_the_captured_volume_password() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::uint32_t hidden_id = session.container_statuses()[0].id;
  session.mount_adopted_container(hidden_id);

  // Card removal releases the volumes and the captured password with them.
  static_cast<void>(session.force_unmount_active());
  require(session.operation_history().empty(),
          "card removal retained the closed session's operation history");
  require_throws<std::logic_error>(
      [&] { session.mount_adopted_container(hidden_id); },
      "the hidden container was mounted after its password was dropped");
}

void test_deliberate_release_keeps_the_captured_volume_password() {
  // A deliberate unmount keeps it so the same session can mount again.
  CardAuthenticationSession authentication;
  FakeRuntime runtime = runtime_with_both_containers();
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  const std::uint32_t hidden_id = session.container_statuses()[0].id;
  session.mount_adopted_container(hidden_id);
  session.unmount_active();
  session.mount_adopted_container(hidden_id);
  require(session.container_statuses()[0].mounted,
          "a deliberate unmount blocked a later mount in the same session");
}

void test_login_adopts_every_mounted_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.mount_a = true;
  runtime.mount_b = true;
  // A real login only mounts containers whose files are present, so the
  // fixture reports both backing files.
  runtime.container_a_exists = true;
  runtime.container_a_size_bytes = 64ULL * 1024ULL * 1024ULL;
  runtime.other_existing.emplace_back(L"C:\\vault\\b.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::mount_on_login);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));

  std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2,
          "login did not adopt both containers into one collection");
  // Adoption order follows the fixed probe order: the hidden container first.
  require(statuses[0].name == L"b.hc" && statuses[0].mounted &&
              statuses[0].drive_letter == L'Y',
          "the hidden container is missing from the collection");
  require(statuses[1].name == L"a.hc" && statuses[1].mounted &&
              statuses[1].drive_letter == L'X',
          "the standard container is missing from the collection");

  // The hidden container is still released before the standard ones.
  runtime.events.clear();
  session.unmount_active();
  require_events(runtime, {"unmount:B", "unmount:A"});
  statuses = session.container_statuses();
  require(statuses.size() == 2 && !statuses[0].mounted &&
              !statuses[1].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              statuses[1].drive_letter == wchar_t{},
          "unmounting dropped the adopted containers or kept a drive");
  require(!session.mounted(), "unmount left an active volume behind");
}

void test_import_falls_back_to_the_login_derivation() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.free_drive_letters = {L'S', L'T'};
  runtime.other_existing.emplace_back(L"D:\\kept.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  runtime.login_derived_path = L"D:\\kept.hc";
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  runtime.events.clear();

  // The user picks a file, never a container class, and the page offers no
  // such choice either. The standard derivation goes first and the login
  // derivation second, so both classes are reached through one entry point.
  const std::uint32_t id = session.mount_container(L"D:\\kept.hc");
  require_events(runtime,
                 {"exists:final-other", "derive:A", "allocate",
                  "mount-created:other", "derive:B", "mount-created:other",
                  "notify:A"});
  require(runtime.last_mount_password ==
              bytes_vector("derived-b-login-password...."),
          "the imported container was opened with the wrong derivation");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == id && statuses[0].mounted &&
              statuses[0].name == L"kept.hc",
          "the imported container was not adopted as mounted");

  // Adoption recorded which derivation opened it, so a later mount in this
  // session no longer probes the standard one.
  runtime.events.clear();
  session.unmount_container(id);
  session.mount_adopted_container(id);
  require_events(runtime, {"unmount:other", "exists:final-other", "allocate",
                           "mount-created:other", "notify:A"});
}

void test_import_reports_a_file_no_derivation_opens() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  runtime.other_existing.emplace_back(L"D:\\foreign.hc");
  runtime.other_size_bytes = 128ULL * 1024ULL * 1024ULL;
  // Neither derivation opens it, so the second failure is the one reported.
  runtime.login_derived_path = L"D:\\foreign.hc";
  runtime.nothing_opens_it = true;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(),
                                      bytes("login-password"));
  runtime.events.clear();

  require_throws<std::system_error>(
      [&] {
        static_cast<void>(session.mount_container(L"D:\\foreign.hc"));
      },
      "a file no derivation opens was reported as mounted");
  require(session.container_statuses().empty(),
          "a failed import still adopted the file");
}

void test_container_b_creation_derives_both_volume_passwords() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  // The hidden volume password is derived from the input the session was
  // opened with, so the caller no longer supplies it.
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();

  const std::uint32_t id = session.create_container_b(
      L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
      16ULL * 1024ULL * 1024ULL, bytes("decoy-password"));

  require(id != 0, "container B creation returned no identifier");
  require(runtime.outer_size == 64ULL * 1024ULL * 1024ULL &&
              runtime.hidden_size == 16ULL * 1024ULL * 1024ULL,
          "container B was created with the wrong sizes");
  require_events(runtime,
                 {"exists:final-other", "exists:stage-other", "derive:B",
                  "derive:B", "create:hidden", "publish:other", "allocate",
                  "mount-created:other", "notify:A"});

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].id == id &&
              statuses[0].mounted && statuses[0].name == L"b.hc",
          "container B was not adopted as a mounted container");
  require(session.mounted(), "container B creation left no active volume");

  // Creation keeps the derived volume password the login path would have
  // produced, so the container it just created can be released and opened
  // again without the user producing anything.
  runtime.events.clear();
  session.unmount_container(id);
  session.mount_adopted_container(id);
  require(session.container_statuses()[0].mounted,
          "a container created in this session could not be mounted again");
}

void test_container_b_creation_rejects_unusable_input() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "unauthenticated container B creation was accepted");

  // Authentication alone is not enough: the derivation needs the input the
  // session was opened with, and a session that never captured one refuses
  // instead of inventing a substitute.
  CardAuthenticationSession bare_authentication;
  FakeRuntime bare_runtime;
  const auto bare = std::make_unique<CardLoginMountSessionCore>(
      bare_authentication, bare_runtime, layout(), LoginMountPolicy::manual);
  bare_authentication.establish(root_secret());
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(bare->create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "container B was created without the input the session captured");
  require(bare_runtime.events.empty(),
          "a session without a captured login still reached the runtime");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();
  for (const auto [outer, hidden] : {
           std::pair<std::uint64_t, std::uint64_t>{0, 0},
           std::pair<std::uint64_t, std::uint64_t>{513, 0},
           std::pair<std::uint64_t, std::uint64_t>{1024, 513},
           std::pair<std::uint64_t, std::uint64_t>{1024, 1024}}) {
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(session.create_container_b(
              L"C:\\vault\\b.hc", outer, hidden,
              bytes("decoy-password")));
        },
        "container B accepted an unusable size pair");
  }
  require_throws<std::invalid_argument>(
      [&] {
        static_cast<void>(session.create_container_b(L"C:\\vault\\b.hc",
                                                     1024, 512,
                                                     SecureBuffer()));
      },
      "container B accepted an empty decoy password");
  require(runtime.events.empty(),
          "a rejected container B request still reached the runtime");
}

void test_container_b_creation_cleans_a_failed_format() {
  FakeRuntime runtime;
  runtime.fail_create = true;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  runtime.events.clear();

  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"C:\\vault\\b.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "a failed container B format was not reported");
  require(session.container_statuses().empty() && !session.mounted(),
          "a failed container B creation left session state behind");
}

void test_hidden_containers_are_unlimited_and_share_one_derivation() {
  FakeRuntime runtime;
  CardAuthenticationSession authentication;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::uint32_t first = session.create_container_b(
      L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL, 16ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));
  runtime.events.clear();
  const std::uint32_t second = session.create_container_b(
      L"E:\\two.hc", 128ULL * 1024ULL * 1024ULL, 32ULL * 1024ULL * 1024ULL,
      bytes("decoy-password"));

  require(first != 0 && second != 0 && first != second,
          "the two containers did not get distinct identifiers");
  // Every container of this kind shares one pair of passwords, so the second
  // creation derives only the decoy side: the login side is still held.
  require_events(runtime,
                 {"exists:final-other", "exists:stage-other", "derive:B",
                  "create:hidden", "publish:other", "allocate",
                  "mount-created:other", "notify:A"});
  require(runtime.hidden_target_paths.size() == 2 &&
              runtime.hidden_target_paths[0] == L"D:\\one.hc.preparing" &&
              runtime.hidden_target_paths[1] == L"E:\\two.hc.preparing",
          "the containers were not created at the caller's own paths");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && statuses[0].name == L"one.hc" &&
              statuses[1].name == L"two.hc" && statuses[0].mounted &&
              statuses[1].mounted &&
              statuses[0].drive_letter != statuses[1].drive_letter,
          "the session did not adopt both containers on distinct drives");

  // A path already adopted is still refused, which is the only count-like
  // limit left.
  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container_b(
            L"D:\\one.hc", 64ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL, bytes("decoy-password")));
      },
      "the same path was accepted twice");
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
  const auto mounted_session = std::make_unique<CardLoginMountSessionCore>(
      mounted_authentication, mounted_runtime, layout(),
      LoginMountPolicy::mount_on_login);
  mounted_session->consume_authenticated_login(root_secret(),
                                               bytes("login-password"));
  mounted_runtime.events.clear();
  const auto mounted = mounted_session->container_a_status();
  require(mounted.exists && mounted.mounted &&
              mounted.size_bytes == 64ULL * 1024ULL * 1024ULL &&
              mounted.drive_letter == L'X',
          "mounted A status did not project its fixed drive and capacity");

  mounted_runtime.container_a_exists = false;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(mounted_session->container_a_status()); },
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
  // Login only looks for the fixed containers so the user can mount them
  // later; it must not mount anything by itself.
  require_events(runtime, {"exists:final-other", "exists:final"});
  require(!session.mounted(),
          "the manual policy reported an active volume after login");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].name == L"a.hc" &&
              !statuses[0].mounted && statuses[0].drive_letter == wchar_t{},
          "manual login did not adopt the existing container unmounted");

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

void test_multiple_standard_containers_get_distinct_drive_letters() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));

  const std::uint32_t first = session.create_container(
      layout().container_a.path, 64ULL * 1024ULL * 1024ULL);
  const std::uint32_t second = session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL);
  require(first != second, "two containers were adopted under one identifier");

  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2, "the session did not adopt both containers");
  require(statuses[0].id == first && statuses[1].id == second,
          "container statuses were not returned in adoption order");
  require(statuses[0].name == L"a.hc" && statuses[1].name == L"second.hc",
          "container statuses did not project the file name");
  require(statuses[0].drive_letter == L'X' &&
              statuses[1].drive_letter == L'W',
          "the two containers did not receive distinct drive letters");
  require(statuses[0].mounted && statuses[1].mounted &&
              statuses[0].size_bytes == 64ULL * 1024ULL * 1024ULL &&
              statuses[1].size_bytes == 64ULL * 1024ULL * 1024ULL,
          "created containers were not projected as mounted with capacity");
  require(session.mounted(), "the session reported no active volume");
}

void test_container_paths_are_adopted_once() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));

  require_throws<std::logic_error>(
      [&] {
        static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));
      },
      "an adopted container path was created a second time");
  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\second.hc")); },
      "an already mounted container was mounted again");
  require(session.container_statuses().size() == 1,
          "a rejected creation still changed the adopted collection");
}

void test_unmounted_container_can_be_mounted_again() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL);

  session.unmount_container(id);
  require(!session.mounted(), "unmounting one container left the session active");
  std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 1 && !statuses[0].mounted &&
              statuses[0].drive_letter == wchar_t{} &&
              statuses[0].size_bytes == 64ULL * 1024ULL * 1024ULL,
          "an unmounted container was not retained as present");

  const std::uint32_t again = session.mount_container(L"C:\\vault\\second.hc");
  require(again == id, "remounting did not reuse the adopted identifier");
  statuses = session.container_statuses();
  require(statuses.size() == 1 && statuses[0].mounted &&
              statuses[0].drive_letter == L'W',
          "remounting did not allocate a fresh drive letter");

  session.unmount_container(id);
  session.unmount_container(id);
  require_throws<std::invalid_argument>(
      [&] { session.unmount_container(id + 100); },
      "an unknown container identifier was accepted");
}

void test_mount_requires_authentication_and_an_existing_file() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);

  require_throws<std::logic_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\second.hc")); },
      "mounting was accepted before authentication");

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  require_throws<std::system_error>(
      [&] { static_cast<void>(session.mount_container(L"C:\\vault\\third.hc")); },
      "a missing container file was mounted");
  require(session.container_statuses().empty(),
          "a failed mount still adopted the container");
}

void test_unmount_active_releases_every_adopted_container() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(authentication, runtime, layout(),
                                    LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  static_cast<void>(session.create_container(layout().container_a.path,
                                             64ULL * 1024ULL * 1024ULL));
  static_cast<void>(session.create_container(L"C:\\vault\\second.hc", 64ULL * 1024ULL * 1024ULL));

  session.unmount_active();

  require(!session.mounted(),
          "unmount_active left an adopted container mounted");
  const std::vector<ContainerStatus> statuses = session.container_statuses();
  require(statuses.size() == 2 && !statuses[0].mounted &&
              !statuses[1].mounted,
          "unmount_active dropped the adopted containers");
}

void test_operation_history_records_only_committed_session_facts() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout(), LoginMountPolicy::manual);

  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 1,
          "opening a session did not replace history with one event");
  require_operation(history[0],
                    1,
                    OperationKind::session_opened,
                    OperationOutcome::succeeded,
                    L"",
                    wchar_t{},
                    "the session-open event exposed a target or wrong result");

  session.clear_operation_history();
  require(session.operation_history().empty(),
          "clearing history recorded another event");
  static_cast<void>(session.create_container(L"C:\\vault\\work.hc",
                                             64ULL * 1024ULL * 1024ULL));
  history = session.operation_history();
  require(history.size() == 2,
          "successful creation was not split into create and mount facts");
  require_operation(history[0],
                    1,
                    OperationKind::container_created,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    wchar_t{},
                    "creation history retained a path or a drive");
  require_operation(history[1],
                    2,
                    OperationKind::container_mounted,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    L'X',
                    "mount history did not retain the confirmed drive");

  session.unmount_container(1);
  authentication.clear_authentication();
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  history = session.operation_history();
  require(history.size() == 1 && history[0].sequence == 1 &&
              history[0].kind == OperationKind::session_opened,
          "a new authenticated session retained the previous history");
}

void test_operation_history_distinguishes_publish_from_mount_failure() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout(), LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  session.clear_operation_history();
  runtime.fail_created_mount = true;

  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(session.create_container(L"C:\\vault\\published.hc",
                                                   64ULL * 1024ULL * 1024ULL));
      },
      "the injected post-publication mount failure was accepted");
  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 2,
          "a partial creation did not produce two authoritative facts");
  require_operation(history[0],
                    1,
                    OperationKind::container_created,
                    OperationOutcome::succeeded,
                    L"published.hc",
                    wchar_t{},
                    "the published container was falsely recorded as failed");
  require_operation(history[1],
                    2,
                    OperationKind::container_mounted,
                    OperationOutcome::failed,
                    L"published.hc",
                    wchar_t{},
                    "the failed mount invented a drive letter");
}

void test_operation_history_records_prepublication_and_unmount_failures() {
  {
    CardAuthenticationSession authentication;
    FakeRuntime runtime;
    const auto session = std::make_unique<CardLoginMountSessionCore>(
        authentication, runtime, layout(), LoginMountPolicy::manual);
    session->consume_authenticated_login(root_secret(),
                                         bytes("login-password"));
    session->clear_operation_history();
    runtime.fail_create = true;
    require_throws<std::runtime_error>(
        [&] {
          static_cast<void>(session->create_container(
              L"C:\\vault\\failed.hc", 64ULL * 1024ULL * 1024ULL));
        },
        "the injected format failure was accepted");
    const std::vector<OperationEvent> history = session->operation_history();
    require(history.size() == 1,
            "a prepublication failure produced a mount event");
    require_operation(history[0],
                      1,
                      OperationKind::container_created,
                      OperationOutcome::failed,
                      L"failed.hc",
                      wchar_t{},
                      "a failed format was not recorded generically");
  }

  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout(), LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\work.hc",
                                                    64ULL * 1024ULL * 1024ULL);
  session.clear_operation_history();
  runtime.fail_unmount_a = true;
  require_throws<std::runtime_error>(
      [&] { session.unmount_container(id); },
      "the injected unmount failure was accepted");
  require(session.mounted(), "a failed unmount cleared the live mounted state");
  runtime.fail_unmount_a = false;
  session.unmount_container(id);

  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == 2,
          "unmount failure and retry were not both retained");
  require_operation(history[0],
                    1,
                    OperationKind::container_unmounted,
                    OperationOutcome::failed,
                    L"work.hc",
                    L'X',
                    "failed unmount history lost its still-active drive");
  require_operation(history[1],
                    2,
                    OperationKind::container_unmounted,
                    OperationOutcome::succeeded,
                    L"work.hc",
                    L'X',
                    "successful unmount history lost its released drive");
}

void test_operation_history_discards_only_the_oldest_events_at_capacity() {
  CardAuthenticationSession authentication;
  FakeRuntime runtime;
  CardLoginMountSessionCore session(
      authentication, runtime, layout(), LoginMountPolicy::manual);
  session.consume_authenticated_login(root_secret(), bytes("login-password"));
  const std::uint32_t id = session.create_container(L"C:\\vault\\bounded.hc",
                                                    64ULL * 1024ULL * 1024ULL);
  session.clear_operation_history();
  runtime.fail_unmount_a = true;

  for (std::size_t attempt = 0; attempt < 130; ++attempt) {
    require_throws<std::runtime_error>(
        [&] { session.unmount_container(id); },
        "the injected bounded-history unmount failure was accepted");
  }

  const std::vector<OperationEvent> history = session.operation_history();
  require(history.size() == usb_vault::application::kOperationHistoryCapacity,
          "operation history exceeded its fixed capacity");
  require(history.front().sequence == 3 &&
              history.front().kind == OperationKind::container_unmounted &&
              history.back().sequence == 130 &&
              history.back().kind == OperationKind::container_unmounted,
          "operation history did not retain the newest events in order");
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
    test_manual_login_adopts_without_deriving_anything();
    test_adopted_containers_mount_without_further_input();
    test_forced_release_drops_the_captured_volume_password();
    test_deliberate_release_keeps_the_captured_volume_password();
    test_login_adopts_every_mounted_container();
    test_import_falls_back_to_the_login_derivation();
    test_import_reports_a_file_no_derivation_opens();
    test_container_b_creation_derives_both_volume_passwords();
    test_container_b_creation_rejects_unusable_input();
    test_container_b_creation_cleans_a_failed_format();
    test_hidden_containers_are_unlimited_and_share_one_derivation();
    test_container_a_status_projects_live_file_and_mount_facts();
    test_manual_policy_authenticates_without_mounting();
    test_manual_policy_still_serializes_and_allows_creation();
    test_multiple_standard_containers_get_distinct_drive_letters();
    test_container_paths_are_adopted_once();
    test_unmounted_container_can_be_mounted_again();
    test_mount_requires_authentication_and_an_existing_file();
    test_unmount_active_releases_every_adopted_container();
    test_operation_history_records_only_committed_session_facts();
    test_operation_history_distinguishes_publish_from_mount_failure();
    test_operation_history_records_prepublication_and_unmount_failures();
    test_operation_history_discards_only_the_oldest_events_at_capacity();
    std::cout << "card_login_mount_session_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_login_mount_session_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
