#include "usb_vault/application/mount_orchestrator.h"

#include "application/mount_orchestrator_internal.h"

#include <array>
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
using usb_vault::application::detail::MountRuntime;
using usb_vault::application::detail::mount_session_with_runtime;
using usb_vault::security::SecureBuffer;

constexpr std::wstring_view kPathA = L"C:\\vault\\a.hc";
constexpr std::wstring_view kPathB = L"C:\\vault\\b.hc";

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

SecureBuffer bytes(const std::string_view value) {
  SecureBuffer result(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(value[index]);
  }
  return result;
}

[[nodiscard]] bool equals(const SecureBuffer& value,
                          const std::string_view expected) {
  if (value.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (value.bytes()[index] != static_cast<std::byte>(expected[index])) {
      return false;
    }
  }
  return true;
}

ContainerLayout layout() {
  ContainerLayout result;
  result.container_a.path = kPathA;
  result.container_a.drive_letter = L'X';
  result.container_a.salt[0] = std::byte{0xA0};
  result.container_b.path = kPathB;
  result.container_b.drive_letter = L'Y';
  result.container_b.salt[0] = std::byte{0xB0};
  return result;
}

class FakeRuntime final : public MountRuntime {
 public:
  bool a_exists{true};
  bool b_exists{true};
  bool fail_b_mount{false};
  bool fail_a_probe{false};
  bool fail_a_derive{false};
  bool fail_a_mount{false};
  bool fail_normal_rollback{false};
  bool fail_force_rollback{false};
  std::vector<std::string> events;

  [[nodiscard]] bool container_exists(
      const std::wstring_view path) override {
    if (path == kPathB) {
      events.emplace_back("probe:B");
      return b_exists;
    }
    if (path == kPathA) {
      events.emplace_back("probe:A");
      if (fail_a_probe) {
        throw std::system_error(std::make_error_code(std::errc::permission_denied),
                                "probe A failed");
      }
      return a_exists;
    }
    throw std::runtime_error("unexpected probe path");
  }

  [[nodiscard]] SecureBuffer derive(
      const SecureBuffer& password_utf8,
      const SecureBuffer&,
      const usb_vault::security::KdfV1Salt&) override {
    const bool fixed_a = equals(password_utf8, "diskA");
    events.emplace_back(fixed_a ? "derive:A" : "derive:user");
    if (fixed_a && fail_a_derive) {
      throw std::runtime_error("derive A failed");
    }
    SecureBuffer result(1);
    result.bytes()[0] = fixed_a ? std::byte{0xA1} : std::byte{0xB1};
    return result;
  }

  [[nodiscard]] usb_vault::volume::MountResult mount(
      const std::wstring_view path,
      const SecureBuffer& volume_password,
      const usb_vault::volume::MountOptions& options) override {
    require(!options.notify_shell, "orchestrator mount was not silent");
    if (path == kPathB) {
      events.emplace_back("mount:B");
      require(volume_password.bytes()[0] == std::byte{0xB1},
              "B did not use the user-password derivation");
      if (fail_b_mount) {
        throw std::system_error(
            usb_vault::volume::make_error_code(
                usb_vault::volume::DriverError::authentication_failed),
            "mount B failed");
      }
      return {};
    }
    if (path == kPathA) {
      events.emplace_back("mount:A");
      const std::byte expected = b_exists ? std::byte{0xA1} : std::byte{0xB1};
      require(volume_password.bytes()[0] == expected,
              "A used the wrong derivation branch");
      if (fail_a_mount) {
        throw std::runtime_error("mount A failed");
      }
      return {};
    }
    throw std::runtime_error("unexpected mount path");
  }

  [[nodiscard]] usb_vault::volume::UnmountResult rollback_normal(
      const wchar_t drive_letter) override {
    require(drive_letter == L'Y', "rollback targeted the wrong drive");
    events.emplace_back("rollback:normal");
    if (fail_normal_rollback) {
      throw std::system_error(
          usb_vault::volume::make_error_code(
              usb_vault::volume::DriverError::volume_busy),
          "normal rollback failed");
    }
    return {};
  }

  [[nodiscard]] usb_vault::volume::UnmountResult rollback_force(
      const wchar_t drive_letter) override {
    require(drive_letter == L'Y', "forced rollback targeted the wrong drive");
    events.emplace_back("rollback:force");
    if (fail_force_rollback) {
      throw std::runtime_error("forced rollback failed");
    }
    return {};
  }

  void notify_mounted(const wchar_t drive_letter) override {
    events.emplace_back(
        drive_letter == L'X' ? "notify:A" : "notify:B");
  }
};

template <typename Exception, typename Function>
void require_throws(Function&& function, const char* const message) {
  try {
    function();
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

void test_b_then_a_success_is_silent_until_complete() {
  FakeRuntime runtime;
  const SecureBuffer password = bytes("real-password");
  const SecureBuffer card = bytes("card");
  const auto result = mount_session_with_runtime(
      runtime, layout(), password, card);

  require(result.container_a.has_value(), "A was not reported mounted");
  require(result.container_b.has_value(), "B was not reported mounted");
  require_events(runtime, {"probe:B", "derive:user", "mount:B", "probe:A",
                           "derive:A", "mount:A", "notify:A", "notify:B"});
}

void test_wrong_b_password_never_touches_a() {
  FakeRuntime runtime;
  runtime.fail_b_mount = true;
  const SecureBuffer password = bytes("wrong-password");
  const SecureBuffer card = bytes("card");

  require_throws<std::system_error>(
      [&] { static_cast<void>(mount_session_with_runtime(
                runtime, layout(), password, card)); },
      "B authentication failure was not propagated");
  require_events(runtime, {"probe:B", "derive:user", "mount:B"});
}

void test_single_container_branches() {
  const SecureBuffer password = bytes("login-password");
  const SecureBuffer card = bytes("card");

  FakeRuntime only_b;
  only_b.a_exists = false;
  const auto b_result = mount_session_with_runtime(
      only_b, layout(), password, card);
  require(!b_result.container_a.has_value() && b_result.container_b.has_value(),
          "B-only result was incorrect");
  require_events(only_b, {"probe:B", "derive:user", "mount:B", "probe:A",
                          "notify:B"});

  FakeRuntime only_a;
  only_a.b_exists = false;
  const auto a_result = mount_session_with_runtime(
      only_a, layout(), password, card);
  require(a_result.container_a.has_value() && !a_result.container_b.has_value(),
          "A-only result was incorrect");
  require_events(only_a, {"probe:B", "probe:A", "derive:user", "mount:A",
                          "notify:A"});

  FakeRuntime neither;
  neither.a_exists = false;
  neither.b_exists = false;
  require_throws<std::system_error>(
      [&] { static_cast<void>(mount_session_with_runtime(
                neither, layout(), password, card)); },
      "missing containers were accepted");
  require_events(neither, {"probe:B", "probe:A"});
}

void test_a_failures_roll_back_b_before_reporting() {
  const SecureBuffer password = bytes("real-password");
  const SecureBuffer card = bytes("card");

  FakeRuntime derive_failure;
  derive_failure.fail_a_derive = true;
  require_throws<std::runtime_error>(
      [&] { static_cast<void>(mount_session_with_runtime(
                derive_failure, layout(), password, card)); },
      "A derivation failure was not propagated");
  require_events(derive_failure,
                 {"probe:B", "derive:user", "mount:B", "probe:A",
                  "derive:A", "rollback:normal"});

  FakeRuntime probe_failure;
  probe_failure.fail_a_probe = true;
  require_throws<std::system_error>(
      [&] { static_cast<void>(mount_session_with_runtime(
                probe_failure, layout(), password, card)); },
      "A probe failure was not propagated");
  require_events(probe_failure,
                 {"probe:B", "derive:user", "mount:B", "probe:A",
                  "rollback:normal"});
}

void test_busy_rollback_forces_once() {
  FakeRuntime runtime;
  runtime.fail_a_mount = true;
  runtime.fail_normal_rollback = true;
  const SecureBuffer password = bytes("real-password");
  const SecureBuffer card = bytes("card");

  require_throws<std::runtime_error>(
      [&] { static_cast<void>(mount_session_with_runtime(
                runtime, layout(), password, card)); },
      "A failure was lost after forced rollback");
  require_events(runtime,
                 {"probe:B", "derive:user", "mount:B", "probe:A",
                  "derive:A", "mount:A", "rollback:normal",
                  "rollback:force"});
}

void test_failed_force_reports_residual_drive() {
  FakeRuntime runtime;
  runtime.fail_a_mount = true;
  runtime.fail_normal_rollback = true;
  runtime.fail_force_rollback = true;
  const SecureBuffer password = bytes("real-password");
  const SecureBuffer card = bytes("card");

  try {
    static_cast<void>(mount_session_with_runtime(runtime, layout(), password,
                                                 card));
  } catch (const MountRollbackError& error) {
    require(error.residual_drive_letter() == L'Y',
            "rollback error lost the residual drive letter");
    require(error.operation_failure() != nullptr,
            "rollback error lost the operation failure");
    require(error.cleanup_failure() != nullptr,
            "rollback error lost the cleanup failure");
    require_events(runtime,
                   {"probe:B", "derive:user", "mount:B", "probe:A",
                    "derive:A", "mount:A", "rollback:normal",
                    "rollback:force"});
    return;
  }
  throw std::runtime_error("failed force rollback was not reported");
}

void test_layout_is_rejected_before_runtime_calls() {
  const SecureBuffer password = bytes("password");
  const SecureBuffer card = bytes("card");

  FakeRuntime same_drive;
  ContainerLayout same_drive_layout = layout();
  same_drive_layout.container_a.drive_letter = L'Y';
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(mount_session_with_runtime(
                same_drive, same_drive_layout, password, card)); },
      "duplicate drive letters were accepted");
  require(same_drive.events.empty(),
          "runtime was touched before drive validation");

  FakeRuntime same_path;
  ContainerLayout same_path_layout = layout();
  same_path_layout.container_a.path = L"C:/VAULT/./B.hc";
  require_throws<std::invalid_argument>(
      [&] { static_cast<void>(mount_session_with_runtime(
                same_path, same_path_layout, password, card)); },
      "duplicate container paths were accepted");
  require(same_path.events.empty(),
          "runtime was touched before path validation");
}

}  // namespace

int main() {
  try {
    test_b_then_a_success_is_silent_until_complete();
    test_wrong_b_password_never_touches_a();
    test_single_container_branches();
    test_a_failures_roll_back_b_before_reporting();
    test_busy_rollback_forces_once();
    test_failed_force_reports_residual_drive();
    test_layout_is_rejected_before_runtime_calls();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
