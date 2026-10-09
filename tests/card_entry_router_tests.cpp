#include "usb_vault/application/card_entry_router.h"

#include <iostream>
#include <stdexcept>
#include <optional>

namespace {

using usb_vault::application::CardEntryMediaEvidence;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::CardEntryRouter;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardRecordState;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    ++media_calls;
    if (throw_on_media) throw std::runtime_error("media read failed");
    return media_evidence;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    ++record_calls;
    if (throw_on_record) throw std::runtime_error("record read failed");
    return record_state;
  }

  CardEntryMediaEvidence media_evidence{CardEntryMediaEvidence::blocked};
  std::optional<usb_vault::card::CardProductRecordState> supported_state;
  usb_vault::card::CardProductRecordState inspect_supported_product_record_read_only() override {
    if (!supported_state) return IReadOnlyCardEntryInspection::inspect_supported_product_record_read_only();
    ++record_calls;
    if (throw_on_record) throw std::runtime_error("supported record read failed");
    return *supported_state;
  }
  CardRecordState record_state{CardRecordState::malformed};
  bool throw_on_media{false};
  bool throw_on_record{false};
  int media_calls{0};
  int record_calls{0};
};

void test_adapter_authorized_new_media_routes_to_initialization() {
  FakeInspection inspection;
  inspection.media_evidence = CardEntryMediaEvidence::initializable_media;

  require(CardEntryRouter::route(inspection) ==
              CardEntryRoute::initialization_required,
          "authorized new media did not route to initialization");
  require(inspection.media_calls == 1 && inspection.record_calls == 0,
          "new-media route inspected a nonexistent product record");
}

void test_blocked_media_never_reaches_record_inspection() {
  FakeInspection inspection;
  inspection.media_evidence = CardEntryMediaEvidence::blocked;
  inspection.record_state = CardRecordState::provisioned_uninitialized;

  require(CardEntryRouter::route(inspection) == CardEntryRoute::unavailable,
          "blocked media reached a usable route");
  require(inspection.media_calls == 1 && inspection.record_calls == 0,
          "blocked media was reinterpreted through record inspection");
}

void test_exact_product_marker_routes_to_initialization() {
  FakeInspection inspection;
  inspection.media_evidence = CardEntryMediaEvidence::product_record;
  inspection.record_state = CardRecordState::provisioned_uninitialized;

  require(CardEntryRouter::route(inspection) ==
              CardEntryRoute::initialization_required,
          "provisioning marker did not route to initialization");
  require(inspection.media_calls == 1 && inspection.record_calls == 1,
          "product marker was not inspected exactly once");
}

void test_initialized_product_record_routes_to_login() {
  FakeInspection inspection;
  inspection.media_evidence = CardEntryMediaEvidence::product_record;
  inspection.record_state = CardRecordState::initialized_v1;

  require(CardEntryRouter::route(inspection) ==
              CardEntryRoute::login_required,
          "initialized record did not route to login");
}

void test_unsafe_product_states_fail_closed() {
  for (const CardRecordState state : {
           CardRecordState::legacy_p1_blob,
           CardRecordState::unsupported_version,
           CardRecordState::malformed,
           static_cast<CardRecordState>(999),
       }) {
    FakeInspection inspection;
    inspection.media_evidence = CardEntryMediaEvidence::product_record;
    inspection.record_state = state;
    require(CardEntryRouter::route(inspection) ==
                CardEntryRoute::unavailable,
            "unsafe product state reached a usable route");
  }
}

void test_unknown_media_evidence_fails_closed() {
  FakeInspection inspection;
  inspection.media_evidence = static_cast<CardEntryMediaEvidence>(999);
  inspection.record_state = CardRecordState::initialized_v1;

  require(CardEntryRouter::route(inspection) == CardEntryRoute::unavailable,
          "unknown media evidence reached a usable route");
  require(inspection.record_calls == 0,
          "unknown media evidence reached record inspection");
}

void test_read_failures_fail_closed_without_retry() {
  FakeInspection media_failure;
  media_failure.throw_on_media = true;
  require(CardEntryRouter::route(media_failure) ==
              CardEntryRoute::unavailable,
          "media failure reached a usable route");
  require(media_failure.media_calls == 1 && media_failure.record_calls == 0,
          "media failure was retried or reached record inspection");

  FakeInspection record_failure;
  record_failure.media_evidence = CardEntryMediaEvidence::product_record;
  record_failure.throw_on_record = true;
  require(CardEntryRouter::route(record_failure) ==
              CardEntryRoute::unavailable,
          "record failure reached a usable route");
  require(record_failure.media_calls == 1 && record_failure.record_calls == 1,
          "record failure was retried");
}

void test_supported_v2_contract_retains_fixed_routes() {
  using State = usb_vault::card::CardProductRecordState;
  for (const auto state : {State::initialized_v2, State::provisioned_uninitialized,
                          State::unsupported_version, State::malformed, static_cast<State>(999)}) {
    FakeInspection inspection;
    inspection.media_evidence = CardEntryMediaEvidence::product_record;
    inspection.record_state = CardRecordState::unsupported_version;
    inspection.supported_state = state;
    const auto expected = state == State::initialized_v2 ? CardEntryRoute::login_required :
        state == State::provisioned_uninitialized ? CardEntryRoute::initialization_required : CardEntryRoute::unavailable;
    require(CardEntryRouter::route(inspection) == expected, "supported format reached incorrect route");
    require(inspection.record_calls == 1 && inspection.media_calls == 1, "supported evidence was inspected twice");
  }
  FakeInspection blocked;
  blocked.supported_state = State::initialized_v2;
  require(CardEntryRouter::route(blocked) == CardEntryRoute::unavailable && blocked.record_calls == 0,
      "supported record bypassed blocked media evidence");
  FakeInspection failed;
  failed.media_evidence = CardEntryMediaEvidence::product_record;
  failed.supported_state = State::initialized_v2;
  failed.throw_on_record = true;
  require(CardEntryRouter::route(failed) == CardEntryRoute::unavailable && failed.record_calls == 1,
      "supported inspection error was retried or admitted");
}

}  // namespace

int main() {
  try {
    test_adapter_authorized_new_media_routes_to_initialization();
    test_blocked_media_never_reaches_record_inspection();
    test_exact_product_marker_routes_to_initialization();
    test_initialized_product_record_routes_to_login();
    test_unsafe_product_states_fail_closed();
    test_unknown_media_evidence_fails_closed();
    test_read_failures_fail_closed_without_retry();
    test_supported_v2_contract_retains_fixed_routes();
    std::cout << "card_entry_router_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_entry_router_tests failed: " << error.what() << '\n';
    return 1;
  }
}
