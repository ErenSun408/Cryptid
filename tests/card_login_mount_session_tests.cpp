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
    CardLoginMountSessionCore session(authentication, runtime, layout());
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
  CardLoginMountSessionCore session(authentication, runtime, layout());

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
  CardLoginMountSessionCore session(authentication, runtime, layout());

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
  CardLoginMountSessionCore session(authentication, runtime, layout());

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
  CardLoginMountSessionCore session(authentication, runtime, layout());
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

}  // namespace

int main() {
  try {
    test_all_container_shapes_establish_authentication();
    test_mount_failures_do_not_revoke_card_authentication();
    test_failed_rollback_retains_only_residual_mount();
    test_invalid_or_duplicate_handoff_fails_before_mount();
    test_unmount_state_is_precise_and_forced_order_is_b_then_a();
    std::cout << "card_login_mount_session_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_login_mount_session_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
