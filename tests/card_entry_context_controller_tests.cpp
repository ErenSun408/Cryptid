#include "usb_vault/application/card_entry_context_controller.h"

#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardEntryContextController;
using usb_vault::application::CardEntryMediaEvidence;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ICardEntryInspectionSource;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardRecordState;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

struct Lifetime final {
  int destroyed{};
  int source_calls{};
  int destroyed_before_source_call{-1};
};

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  FakeInspection(Lifetime& lifetime, const CardEntryMediaEvidence media,
                 const CardRecordState record)
      : lifetime_(lifetime), media_(media), record_(record) {}

  ~FakeInspection() noexcept override { ++lifetime_.destroyed; }

  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    return media_;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    return record_;
  }

 private:
  Lifetime& lifetime_;
  CardEntryMediaEvidence media_;
  CardRecordState record_;
};

struct InspectionSpec final {
  CardEntryMediaEvidence media{CardEntryMediaEvidence::blocked};
  CardRecordState record{CardRecordState::malformed};
};

// Minimal card behind an update target: it holds one record and reports the
// same bytes back, which is all the entry controller needs to route through
// the record service.
class FakeRecordStore final : public usb_vault::card::ICardStore {
 public:
  [[nodiscard]] SecureBuffer get_blob() override {
    SecureBuffer result(blob.size());
    std::ranges::copy(blob, result.bytes().begin());
    return result;
  }

  void put_blob(const SecureBuffer& value) override {
    ++put_calls;
    blob.assign(value.bytes().begin(), value.bytes().end());
  }

  [[nodiscard]] usb_vault::card::CardCapabilities capabilities()
      const noexcept override {
    return {};
  }

  std::vector<std::byte> blob;
  int put_calls{};
};

class FakeUpdateTarget final : public usb_vault::card::ICardUpdateTarget {
 public:
  explicit FakeUpdateTarget(FakeRecordStore& store) noexcept
      : store_(store) {}

  [[nodiscard]] usb_vault::card::ICardStore& card_store() noexcept override {
    return store_;
  }

 private:
  FakeRecordStore& store_;
};

class FakeSource final : public ICardEntryInspectionSource {
 public:
  [[nodiscard]] std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() override {
    ++lifetime.source_calls;
    lifetime.destroyed_before_source_call = lifetime.destroyed;
    if (throw_on_call) throw std::runtime_error("inspection failed");
    if (return_null) return nullptr;
    if (next >= specs.size()) {
      throw std::runtime_error("missing fake inspection");
    }
    const InspectionSpec spec = specs[next++];
    return std::make_unique<FakeInspection>(lifetime, spec.media,
                                            spec.record);
  }

  [[nodiscard]] std::unique_ptr<usb_vault::card::ICardUpdateTarget>
  make_update_target(const IReadOnlyCardEntryInspection&) override {
    ++update_target_calls;
    if (!support_updates) return nullptr;
    return std::make_unique<FakeUpdateTarget>(record);
  }

  Lifetime lifetime;
  std::vector<InspectionSpec> specs;
  std::size_t next{};
  bool throw_on_call{false};
  bool return_null{false};
  bool support_updates{false};
  int update_target_calls{};
  FakeRecordStore record;
};

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

void test_second_card_password_requires_the_login_route() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::initializable_media,
                          CardRecordState::provisioned_uninitialized});
  CardEntryContextController controller(source);
  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "arrival reached the wrong route");

  bool rejected = false;
  try {
    controller.set_second_card_password(secure_text("existing"),
                                        secure_text("second"));
  } catch (const std::system_error&) {
    rejected = true;
  }
  require(rejected && source.update_target_calls == 0,
          "a record update was attempted outside the login route");
  require(controller.current_route() ==
                  CardEntryRoute::initialization_required &&
              controller.has_context(),
          "a rejected route check disturbed the insertion");
}

void test_second_card_password_rejects_empty_input_without_retiring() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::product_record,
                          CardRecordState::initialized_v1});
  CardEntryContextController controller(source);
  static_cast<void>(controller.handle_arrival());

  bool rejected = false;
  try {
    controller.set_second_card_password(SecureBuffer(),
                                        secure_text("second"));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected && source.update_target_calls == 0,
          "an empty password still reached the adapter");
  require(controller.current_route() == CardEntryRoute::login_required &&
              controller.has_context(),
          "an input mistake retired a healthy insertion");
}

void test_second_card_password_retires_the_insertion_on_failure() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::product_record,
                          CardRecordState::initialized_v1});
  CardEntryContextController controller(source);
  static_cast<void>(controller.handle_arrival());

  bool reported = false;
  try {
    controller.set_second_card_password(secure_text("existing"),
                                        secure_text("second"));
  } catch (const std::system_error&) {
    reported = true;
  }
  require(reported && source.update_target_calls == 1,
          "an unsupported adapter did not fail closed");
  require(controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "a failed record update kept an insertion of unknown state");
}

void test_second_card_password_rewrites_the_record_once() {
  FakeSource source;
  source.support_updates = true;
  source.specs.push_back({CardEntryMediaEvidence::product_record,
                          CardRecordState::initialized_v1});
  SecureBuffer initialized = usb_vault::card::initialize_card_record_v1(
      secure_text("entry-daily-password"));
  source.record.blob.assign(initialized.bytes().begin(),
                            initialized.bytes().end());
  CardEntryContextController controller(source);
  static_cast<void>(controller.handle_arrival());

  controller.set_second_card_password(secure_text("entry-daily-password"),
                                      secure_text("entry-second-password"));
  require(source.record.put_calls == 1,
          "the record update did not write the card exactly once");

  SecureBuffer stored(source.record.blob.size());
  std::ranges::copy(source.record.blob, stored.bytes().begin());
  SecureBuffer existing_root = usb_vault::card::unlock_card_record_v1(
      stored.bytes(), secure_text("entry-daily-password"));
  SecureBuffer second_root = usb_vault::card::unlock_card_record_v1(
      stored.bytes(), secure_text("entry-second-password"));
  require(std::ranges::equal(existing_root.bytes(), second_root.bytes()),
          "the rewritten record no longer opens with both passwords");
  require(controller.current_route() == CardEntryRoute::login_required &&
              controller.has_context(),
          "a successful record update retired the insertion");
}

void test_starts_without_card_or_context() {
  FakeSource source;
  CardEntryContextController controller(source);
  require(!controller.current_route().has_value(),
          "entry controller started with a route");
  require(!controller.has_context(),
          "entry controller started with a context");
  require(source.lifetime.source_calls == 0,
          "entry controller inspected without an arrival");
}

void test_arrival_routes_once_and_retains_context() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::product_record,
                          CardRecordState::initialized_v1});
  CardEntryContextController controller(source);

  require(controller.handle_arrival() == CardEntryRoute::login_required,
          "initialized arrival reached the wrong route");
  require(controller.current_route() == CardEntryRoute::login_required,
          "entry controller did not retain the fixed route");
  require(controller.has_context(),
          "entry controller discarded a valid context");
  require(source.lifetime.source_calls == 1 &&
              source.lifetime.destroyed == 0,
          "arrival repeated inspection or destroyed its live context");
}

void test_new_arrival_destroys_old_binding_before_inspection() {
  FakeSource source;
  source.specs = {
      {CardEntryMediaEvidence::initializable_media,
       CardRecordState::malformed},
      {CardEntryMediaEvidence::product_record,
       CardRecordState::initialized_v1},
  };
  CardEntryContextController controller(source);

  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "first arrival reached the wrong route");
  require(controller.handle_arrival() == CardEntryRoute::login_required,
          "replacement arrival reached the wrong route");
  require(source.lifetime.source_calls == 2 &&
              source.lifetime.destroyed_before_source_call == 1,
          "replacement inspection began before old context destruction");
  require(source.lifetime.destroyed == 1 && controller.has_context(),
          "replacement did not retain exactly one live context");
}

void test_removal_destroys_context_and_clears_route() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::blocked,
                          CardRecordState::malformed});
  CardEntryContextController controller(source);
  require(controller.handle_arrival() == CardEntryRoute::unavailable,
          "blocked arrival reached a usable route");
  require(controller.has_context(),
          "blocked evidence context was not retained for its insertion");

  controller.handle_removal();
  require(source.lifetime.destroyed == 1,
          "removal did not destroy the insertion context");
  require(!controller.current_route().has_value() &&
              !controller.has_context(),
          "removal retained route or context state");

  controller.handle_removal();
  require(source.lifetime.destroyed == 1,
          "duplicate removal changed cleared state");
}

void test_controller_destruction_releases_live_context() {
  FakeSource source;
  source.specs.push_back({CardEntryMediaEvidence::product_record,
                          CardRecordState::initialized_v1});
  {
    CardEntryContextController controller(source);
    require(controller.handle_arrival() == CardEntryRoute::login_required &&
                controller.has_context(),
            "shutdown fixture did not retain a live context");
  }
  require(source.lifetime.destroyed == 1,
          "controller destruction retained its insertion context");
}

void test_source_failures_fail_closed_without_context() {
  FakeSource throwing_source;
  throwing_source.specs.push_back({CardEntryMediaEvidence::product_record,
                                   CardRecordState::initialized_v1});
  CardEntryContextController throwing(throwing_source);
  require(throwing.handle_arrival() == CardEntryRoute::login_required,
          "failure fixture did not establish its old context");
  throwing_source.throw_on_call = true;
  require(throwing.handle_arrival() == CardEntryRoute::unavailable,
          "source exception reached a usable route");
  require(throwing.current_route() == CardEntryRoute::unavailable &&
              !throwing.has_context() &&
              throwing_source.lifetime.source_calls == 2 &&
              throwing_source.lifetime.destroyed_before_source_call == 1 &&
              throwing_source.lifetime.destroyed == 1,
          "source exception retried or retained the old context");

  FakeSource null_source;
  null_source.return_null = true;
  CardEntryContextController null_context(null_source);
  require(null_context.handle_arrival() == CardEntryRoute::unavailable,
          "null source result reached a usable route");
  require(null_context.current_route() == CardEntryRoute::unavailable &&
              !null_context.has_context() &&
              null_source.lifetime.source_calls == 1,
          "null source result was retried or retained a context");
}

}  // namespace

int main() {
  try {
    test_second_card_password_requires_the_login_route();
  test_second_card_password_rejects_empty_input_without_retiring();
  test_second_card_password_retires_the_insertion_on_failure();
  test_second_card_password_rewrites_the_record_once();
  test_starts_without_card_or_context();
    test_arrival_routes_once_and_retains_context();
    test_new_arrival_destroys_old_binding_before_inspection();
    test_removal_destroys_context_and_clears_route();
    test_controller_destruction_releases_live_context();
    test_source_failures_fail_closed_without_context();
    std::cout << "card_entry_context_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_entry_context_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
