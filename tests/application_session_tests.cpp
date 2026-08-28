#include "usb_vault/application/application_session.h"

#include "application/application_session_internal.h"
#include "usb_vault/security/kdf_v1.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::ContainerLayout;
using usb_vault::application::MountRollbackError;
using usb_vault::application::MountSessionResult;
using usb_vault::application::detail::ApplicationSessionRuntime;
using usb_vault::application::detail::ApplicationSessionState;
using usb_vault::application::detail::authenticate_and_mount_with_runtime;
using usb_vault::application::detail::force_unmount_active_with_runtime;
using usb_vault::application::detail::unmount_active_with_runtime;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(text[index]);
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

class FakeApplicationSessionRuntime final : public ApplicationSessionRuntime {
 public:
  std::size_t card_blob_size{
      usb_vault::security::kKdfV1CardSecretSize};
  bool mount_a{true};
  bool mount_b{true};
  bool fail_mount{false};
  bool leave_b_after_mount_failure{false};
  bool fail_unmount_a{false};
  bool fail_unmount_b{false};
  bool fail_force_unmount_a{false};
  bool fail_force_unmount_b{false};
  bool force_required_a{false};
  bool force_required_b{false};
  std::vector<std::string> events;

  [[nodiscard]] SecureBuffer get_card_blob() override {
    events.emplace_back("card:get");
    SecureBuffer result(card_blob_size);
    for (std::size_t index = 0; index < result.size(); ++index) {
      result.bytes()[index] = static_cast<std::byte>(index);
    }
    return result;
  }

  [[nodiscard]] MountSessionResult mount(
      const ContainerLayout& received_layout,
      const SecureBuffer& password_utf8,
      const SecureBuffer& card_secret) override {
    events.emplace_back("mount");
    require(received_layout.container_a.drive_letter == L'X' &&
                received_layout.container_b.drive_letter == L'Y',
            "mount received the wrong layout");
    require(!password_utf8.empty(), "mount received an empty password");
    require(card_secret.size() ==
                usb_vault::security::kKdfV1CardSecretSize,
            "mount received the wrong card blob size");

    if (leave_b_after_mount_failure) {
      throw MountRollbackError(
          L'Y', std::make_exception_ptr(std::runtime_error("mount failed")),
          std::make_exception_ptr(std::runtime_error("rollback failed")));
    }
    if (fail_mount) {
      throw std::runtime_error("mount failed");
    }

    MountSessionResult result;
    if (mount_a) {
      result.container_a.emplace();
    }
    if (mount_b) {
      result.container_b.emplace();
    }
    return result;
  }

  [[nodiscard]] usb_vault::volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    if (drive_letter == L'Y') {
      events.emplace_back("unmount:B");
      if (fail_unmount_b) {
        throw std::runtime_error("unmount B failed");
      }
      return {};
    }
    if (drive_letter == L'X') {
      events.emplace_back("unmount:A");
      if (fail_unmount_a) {
        throw std::runtime_error("unmount A failed");
      }
      return {};
    }
    throw std::runtime_error("unexpected drive letter");
  }

  [[nodiscard]] usb_vault::volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    if (drive_letter == L'Y') {
      events.emplace_back("force-unmount:B");
      if (fail_force_unmount_b) {
        throw std::runtime_error("forced unmount B failed");
      }
      return {.result = {}, .forced = force_required_b};
    }
    if (drive_letter == L'X') {
      events.emplace_back("force-unmount:A");
      if (fail_force_unmount_a) {
        throw std::runtime_error("forced unmount A failed");
      }
      return {.result = {}, .forced = force_required_a};
    }
    throw std::runtime_error("unexpected drive letter");
  }
};

void require_events(
    const FakeApplicationSessionRuntime& runtime,
    const std::initializer_list<std::string_view> expected) {
  require(runtime.events.size() == expected.size(),
          "runtime event count differed");
  std::size_t index = 0;
  for (const std::string_view event : expected) {
    require(runtime.events[index] == event, "runtime event order differed");
    ++index;
  }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, const char* const message) {
  try {
    function();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

void test_mount_consumes_card_blob_and_tracks_only_active_state() {
  FakeApplicationSessionRuntime runtime;
  ApplicationSessionState state;

  authenticate_and_mount_with_runtime(
      runtime, layout(), state, secure_text("login-password"));

  require(state.container_a_mounted && state.container_b_mounted,
          "successful mount state differed");
  require_events(runtime, {"card:get", "mount"});
}

void test_invalid_card_blob_fails_before_mount() {
  FakeApplicationSessionRuntime runtime;
  runtime.card_blob_size =
      usb_vault::security::kKdfV1CardSecretSize - 1;
  ApplicationSessionState state;

  require_throws<std::invalid_argument>(
      [&] {
        authenticate_and_mount_with_runtime(
            runtime, layout(), state, secure_text("login-password"));
      },
      "invalid card blob size was accepted");
  require(!state.mounted(), "invalid card blob left mounted state");
  require_events(runtime, {"card:get"});
}

void test_mount_failure_does_not_invent_state() {
  FakeApplicationSessionRuntime runtime;
  runtime.fail_mount = true;
  ApplicationSessionState state;

  require_throws<std::runtime_error>(
      [&] {
        authenticate_and_mount_with_runtime(
            runtime, layout(), state, secure_text("wrong-password"));
      },
      "mount failure was not propagated");
  require(!state.mounted(), "ordinary mount failure left mounted state");
  require_events(runtime, {"card:get", "mount"});
}

void test_residual_rollback_state_can_be_unmounted() {
  FakeApplicationSessionRuntime runtime;
  runtime.leave_b_after_mount_failure = true;
  ApplicationSessionState state;

  require_throws<MountRollbackError>(
      [&] {
        authenticate_and_mount_with_runtime(
            runtime, layout(), state, secure_text("login-password"));
      },
      "residual rollback failure was not propagated");
  require(!state.container_a_mounted && state.container_b_mounted,
          "residual drive state was not retained");

  runtime.leave_b_after_mount_failure = false;
  unmount_active_with_runtime(runtime, layout(), state);
  require(!state.mounted(), "residual drive was not cleared");
  require_events(runtime, {"card:get", "mount", "unmount:B"});
}

void test_active_unmount_orders_b_before_a() {
  FakeApplicationSessionRuntime runtime;
  ApplicationSessionState state{.container_a_mounted = true,
                                .container_b_mounted = true};

  unmount_active_with_runtime(runtime, layout(), state);

  require(!state.mounted(), "active unmount retained state");
  require_events(runtime, {"unmount:B", "unmount:A"});
}

void test_failed_b_unmount_preserves_both_for_retry() {
  FakeApplicationSessionRuntime runtime;
  runtime.fail_unmount_b = true;
  ApplicationSessionState state{.container_a_mounted = true,
                                .container_b_mounted = true};

  require_throws<std::runtime_error>(
      [&] { unmount_active_with_runtime(runtime, layout(), state); },
      "B unmount failure was not propagated");
  require(state.container_a_mounted && state.container_b_mounted,
          "B failure changed active state");

  runtime.fail_unmount_b = false;
  unmount_active_with_runtime(runtime, layout(), state);
  require(!state.mounted(), "retry did not clear both drives");
  require_events(runtime, {"unmount:B", "unmount:B", "unmount:A"});
}

void test_failed_a_unmount_retains_only_a_for_retry() {
  FakeApplicationSessionRuntime runtime;
  runtime.fail_unmount_a = true;
  ApplicationSessionState state{.container_a_mounted = true,
                                .container_b_mounted = true};

  require_throws<std::runtime_error>(
      [&] { unmount_active_with_runtime(runtime, layout(), state); },
      "A unmount failure was not propagated");
  require(state.container_a_mounted && !state.container_b_mounted,
          "A failure retained the wrong drive state");

  runtime.fail_unmount_a = false;
  unmount_active_with_runtime(runtime, layout(), state);
  require(!state.mounted(), "A retry did not clear state");
  require_events(runtime, {"unmount:B", "unmount:A", "unmount:A"});
}

void test_forced_unmount_orders_b_before_a_and_reports_force() {
  FakeApplicationSessionRuntime runtime;
  ApplicationSessionState state{.container_a_mounted = true,
                                .container_b_mounted = true};
  require(!force_unmount_active_with_runtime(runtime, layout(), state),
          "an idle forced unmount claimed that force was required");
  require(!state.mounted(), "forced unmount retained state");
  require_events(runtime, {"force-unmount:B", "force-unmount:A"});

  FakeApplicationSessionRuntime busy;
  busy.force_required_a = true;
  ApplicationSessionState busy_state{.container_a_mounted = true,
                                     .container_b_mounted = true};
  require(force_unmount_active_with_runtime(busy, layout(), busy_state),
          "a required forced unmount was not reported");
  require(!busy_state.mounted(), "busy forced unmount retained state");
}

void test_failed_forced_unmount_preserves_the_exact_drive() {
  FakeApplicationSessionRuntime runtime;
  runtime.fail_force_unmount_b = true;
  ApplicationSessionState state{.container_a_mounted = true,
                                .container_b_mounted = true};
  require_throws<std::runtime_error>(
      [&] {
        static_cast<void>(
            force_unmount_active_with_runtime(runtime, layout(), state));
      },
      "forced B unmount failure was not propagated");
  require(state.container_a_mounted && state.container_b_mounted,
          "forced B unmount failure lost retryable state");
  require_events(runtime, {"force-unmount:B"});
}

void test_second_mount_is_rejected_before_card_access() {
  FakeApplicationSessionRuntime runtime;
  ApplicationSessionState state{.container_a_mounted = true};

  require_throws<std::logic_error>(
      [&] {
        authenticate_and_mount_with_runtime(
            runtime, layout(), state, secure_text("login-password"));
      },
      "second mount was accepted");
  require_events(runtime, {});
}

}  // namespace

int main() {
  test_mount_consumes_card_blob_and_tracks_only_active_state();
  test_invalid_card_blob_fails_before_mount();
  test_mount_failure_does_not_invent_state();
  test_residual_rollback_state_can_be_unmounted();
  test_active_unmount_orders_b_before_a();
  test_failed_b_unmount_preserves_both_for_retry();
  test_failed_a_unmount_retains_only_a_for_retry();
  test_forced_unmount_orders_b_before_a_and_reports_force();
  test_failed_forced_unmount_preserves_the_exact_drive();
  test_second_mount_is_rejected_before_card_access();

  std::cout << "application_session_tests: PASS\n";
  return 0;
}
