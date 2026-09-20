#include "usb_vault/application/card_device_lifecycle_controller.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardDeviceLifecycleController;
using usb_vault::application::CardEntryContextController;
using usb_vault::application::CardEntryMediaEvidence;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::CardPresenceController;
using usb_vault::application::CardPresenceTransition;
using usb_vault::application::IActiveVolumeSession;
using usb_vault::application::ICardEntryInspectionSource;
using usb_vault::application::IVolatileCardAuthenticationSession;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardRecordState;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSession final : public IActiveVolumeSession {
 public:
  explicit FakeSession(std::vector<std::string>& calls) : calls_(calls) {}

  void unmount_active() override { calls_.push_back("unmount"); }
  [[nodiscard]] bool force_unmount_active() override {
    calls_.push_back("force-unmount");
    if (throw_on_force) throw std::runtime_error("force unmount failed");
    mounted_state = false;
    return force_required;
  }
  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  bool mounted_state{};
  bool force_required{};
  bool throw_on_force{};

 private:
  std::vector<std::string>& calls_;
};

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  explicit FakeInspection(std::vector<std::string>& calls)
      : calls_(calls) {}
  ~FakeInspection() noexcept override {
    calls_.push_back("destroy-context");
  }

  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    calls_.push_back("inspect-media");
    return CardEntryMediaEvidence::product_record;
  }
  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    calls_.push_back("inspect-record");
    return CardRecordState::initialized_v1;
  }

 private:
  std::vector<std::string>& calls_;
};

class FakeAuthenticationSession final
    : public IVolatileCardAuthenticationSession {
 public:
  explicit FakeAuthenticationSession(std::vector<std::string>& calls)
      : calls_(calls) {}

  void clear_authentication() noexcept override {
    calls_.push_back("clear-authentication");
    authenticated_state = false;
  }

  [[nodiscard]] bool authenticated() const noexcept override {
    return authenticated_state;
  }

  bool authenticated_state{};

 private:
  std::vector<std::string>& calls_;
};

class FakeSource final : public ICardEntryInspectionSource {
 public:
  explicit FakeSource(std::vector<std::string>& calls) : calls_(calls) {}

  [[nodiscard]] std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() override {
    calls_.push_back("source");
    ++calls_count;
    if (fail) throw std::runtime_error("source failed");
    return std::make_unique<FakeInspection>(calls_);
  }

  int calls_count{};
  bool fail{};

 private:
  std::vector<std::string>& calls_;
};

struct Fixture final {
  Fixture()
      : session(calls), authentication(calls), source(calls),
        presence(session), entry(source),
        lifecycle(presence, entry, authentication) {}

  std::vector<std::string> calls;
  FakeSession session;
  FakeAuthenticationSession authentication;
  FakeSource source;
  CardPresenceController presence;
  CardEntryContextController entry;
  CardDeviceLifecycleController lifecycle;
};

void test_arrival_marks_present_and_inspects_once() {
  Fixture fixture;
  fixture.authentication.authenticated_state = true;
  const auto transition = fixture.lifecycle.handle_arrival();
  require(transition.presence == CardPresenceTransition::card_available &&
              transition.entry_route == CardEntryRoute::login_required &&
              transition.entry_changed,
          "first arrival returned the wrong lifecycle transition");
  require(fixture.presence.card_present() && fixture.entry.has_context(),
          "first arrival did not retain presence and context");
  require(fixture.calls == std::vector<std::string>{
                               "clear-authentication", "source",
                               "inspect-media", "inspect-record"},
          "arrival did not clear stale authentication before inspection");
  require(!fixture.authentication.authenticated(),
          "arrival retained stale authentication");

  const auto duplicate = fixture.lifecycle.handle_arrival();
  require(duplicate.presence == CardPresenceTransition::no_change &&
              duplicate.entry_route == CardEntryRoute::login_required &&
              !duplicate.entry_changed && fixture.source.calls_count == 1,
          "duplicate arrival repeated the read-only inspection");
}

void test_source_failure_is_unavailable_but_present() {
  Fixture fixture;
  fixture.authentication.authenticated_state = true;
  fixture.source.fail = true;
  const auto transition = fixture.lifecycle.handle_arrival();
  require(transition.presence == CardPresenceTransition::card_available &&
              transition.entry_route == CardEntryRoute::unavailable &&
              transition.entry_changed,
          "source failure returned the wrong transition");
  require(fixture.presence.card_present() && !fixture.entry.has_context() &&
              fixture.source.calls_count == 1 &&
              !fixture.authentication.authenticated(),
          "source failure retried or lost physical presence");
  const auto duplicate = fixture.lifecycle.handle_arrival();
  require(duplicate.presence == CardPresenceTransition::no_change &&
              duplicate.entry_route == CardEntryRoute::unavailable &&
              !duplicate.entry_changed && fixture.source.calls_count == 1,
          "duplicate arrival retried a failed source");
  require(fixture.entry.startup_failure().primary.code == 9001,
          "duplicate arrival discarded the failure");
  static_cast<void>(fixture.lifecycle.handle_removal());
  require(fixture.entry.startup_failure().primary.code == 0,
          "removal retained the failed insertion diagnostic");
  fixture.source.fail = false;
  const auto recovered = fixture.lifecycle.handle_arrival();
  require(recovered.entry_route == CardEntryRoute::login_required &&
              fixture.source.calls_count == 2 &&
              fixture.entry.startup_failure().primary.code == 0,
          "new insertion failed to retire the previous error");
}

void test_removal_destroys_context_before_forced_unmount() {
  Fixture fixture;
  static_cast<void>(fixture.lifecycle.handle_arrival());
  fixture.calls.clear();
  fixture.session.mounted_state = true;
  fixture.authentication.authenticated_state = true;
  fixture.session.force_required = true;

  const auto transition = fixture.lifecycle.handle_removal();
  require(transition.presence ==
              CardPresenceTransition::active_session_force_unmounted &&
              !transition.entry_route.has_value() &&
              transition.entry_changed,
          "removal returned the wrong lifecycle transition");
  require(fixture.calls ==
              std::vector<std::string>{"destroy-context",
                                       "clear-authentication",
                                       "force-unmount"},
          "removal used the wrong context, secret, and unmount order");
  require(!fixture.presence.card_present() && !fixture.entry.has_context(),
          "removal retained presence or context");
  require(!fixture.authentication.authenticated(),
          "removal retained root authentication");
}

void test_duplicate_removal_is_idempotent() {
  Fixture fixture;
  static_cast<void>(fixture.lifecycle.handle_arrival());
  static_cast<void>(fixture.lifecycle.handle_removal());
  fixture.calls.clear();

  const auto transition = fixture.lifecycle.handle_removal();
  require(transition.presence == CardPresenceTransition::no_change &&
              !transition.entry_route.has_value() &&
              !transition.entry_changed &&
              fixture.calls ==
                  std::vector<std::string>{"clear-authentication"} &&
              !fixture.authentication.authenticated(),
          "duplicate removal changed cleared lifecycle state");
}

void test_failed_unmount_cannot_restore_removed_context() {
  Fixture fixture;
  static_cast<void>(fixture.lifecycle.handle_arrival());
  fixture.calls.clear();
  fixture.session.mounted_state = true;
  fixture.authentication.authenticated_state = true;
  fixture.session.throw_on_force = true;

  const auto transition = fixture.lifecycle.handle_removal();
  require(transition.presence ==
              CardPresenceTransition::active_session_remains &&
              !transition.entry_route.has_value() &&
              transition.entry_changed,
          "failed unmount returned the wrong lifecycle transition");
  require(fixture.calls ==
              std::vector<std::string>{"destroy-context",
                                       "clear-authentication",
                                       "force-unmount"} &&
              !fixture.entry.has_context() && fixture.session.mounted_state &&
              !fixture.authentication.authenticated(),
          "failed unmount restored removed context or root authentication");
}

}  // namespace

int main() {
  try {
    test_arrival_marks_present_and_inspects_once();
    test_source_failure_is_unavailable_but_present();
    test_removal_destroys_context_before_forced_unmount();
    test_duplicate_removal_is_idempotent();
    test_failed_unmount_cannot_restore_removed_context();
    std::cout << "card_device_lifecycle_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_device_lifecycle_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
