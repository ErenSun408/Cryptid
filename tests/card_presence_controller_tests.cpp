#include "usb_vault/application/card_presence_controller.h"

#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::application::CardPresenceController;
using usb_vault::application::CardPresenceTransition;
using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer) override {
    ++authenticate_calls;
    is_mounted = true;
  }

  void unmount_active() override {
    ++unmount_calls;
    if (throw_on_unmount) throw std::runtime_error("busy");
    if (!keep_mounted_after_unmount) is_mounted = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    ++force_unmount_calls;
    if (throw_on_unmount) throw std::runtime_error("busy");
    if (!keep_mounted_after_unmount) is_mounted = false;
    return force_required;
  }

  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool is_mounted{false};
  bool throw_on_unmount{false};
  bool keep_mounted_after_unmount{false};
  bool force_required{false};
  int authenticate_calls{0};
  int unmount_calls{0};
  int force_unmount_calls{0};
};

void test_arrival_only_changes_volatile_presence() {
  FakeSession session;
  CardPresenceController controller(session);
  require(!controller.card_present(), "card started present unexpectedly");
  require(controller.handle_arrival() ==
              CardPresenceTransition::card_available,
          "first arrival returned the wrong transition");
  require(controller.card_present(), "arrival did not mark the card present");
  require(session.authenticate_calls == 0 && session.unmount_calls == 0 &&
              session.force_unmount_calls == 0,
          "arrival performed a session action");
  require(controller.handle_arrival() == CardPresenceTransition::no_change,
          "duplicate arrival was not ignored");
}

void test_inactive_removal_is_idempotent() {
  FakeSession session;
  CardPresenceController controller(session, true);
  require(controller.handle_removal() ==
              CardPresenceTransition::card_unavailable,
          "inactive removal returned the wrong transition");
  require(!controller.card_present(), "removal retained card presence");
  require(session.unmount_calls == 0 && session.force_unmount_calls == 0,
          "inactive removal attempted to unmount");
  require(controller.handle_removal() == CardPresenceTransition::no_change,
          "duplicate removal was not ignored");
}

void test_active_removal_unmounts_once() {
  FakeSession session;
  session.is_mounted = true;
  CardPresenceController controller(session, true);
  require(controller.handle_removal() ==
              CardPresenceTransition::active_session_unmounted,
          "successful removal returned the wrong transition");
  require(!session.is_mounted && session.force_unmount_calls == 1,
          "active removal did not unmount exactly once");
  require(session.unmount_calls == 0,
          "removal used the retrying unmount instead of the forced one");
  require(controller.handle_removal() == CardPresenceTransition::no_change,
          "duplicate removal retried a completed transition");
  require(session.force_unmount_calls == 1,
          "duplicate removal repeated the unmount");
}

void test_busy_removal_escalates_to_a_forced_unmount() {
  FakeSession session;
  session.is_mounted = true;
  session.force_required = true;
  CardPresenceController controller(session, true);
  require(controller.handle_removal() ==
              CardPresenceTransition::active_session_force_unmounted,
          "a forced unmount was reported as an ordinary one");
  require(!session.is_mounted && session.force_unmount_calls == 1,
          "forced removal left the session mounted");
}

void test_unmount_exception_preserves_retryable_session() {
  FakeSession session;
  session.is_mounted = true;
  session.throw_on_unmount = true;
  CardPresenceController controller(session, true);
  require(controller.handle_removal() ==
              CardPresenceTransition::active_session_remains,
          "unmount exception was reported as success");
  require(session.is_mounted && session.force_unmount_calls == 1,
          "unmount exception lost retryable state");
  require(!controller.card_present(),
          "failed unmount retained physical card presence");
  require(controller.handle_removal() == CardPresenceTransition::no_change,
          "duplicate removal retried a failed transition");
  require(session.force_unmount_calls == 1,
          "duplicate removal repeated a failed unmount");
}

void test_nonclearing_unmount_fails_closed() {
  FakeSession session;
  session.is_mounted = true;
  session.keep_mounted_after_unmount = true;
  CardPresenceController controller(session, true);
  require(controller.handle_removal() ==
              CardPresenceTransition::active_session_remains,
          "mounted state after unmount was reported as success");
  require(session.is_mounted && session.force_unmount_calls == 1,
          "nonclearing unmount changed the wrong state");
}

}  // namespace

int main() {
  try {
    test_arrival_only_changes_volatile_presence();
    test_inactive_removal_is_idempotent();
    test_active_removal_unmounts_once();
    test_busy_removal_escalates_to_a_forced_unmount();
    test_unmount_exception_preserves_retryable_session();
    test_nonclearing_unmount_fails_closed();
    std::cout << "card_presence_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_presence_controller_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
