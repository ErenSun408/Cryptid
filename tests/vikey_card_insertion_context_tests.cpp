#include "card/vikey/vikey_card_insertion_context.h"

#include "usb_vault/application/card_entry_router.h"
#include "usb_vault/card/card_error.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::application::CardEntryRoute;
using usb_vault::application::CardEntryRouter;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::CardProductRecordState;
using usb_vault::card::vikey::VikeyCardInsertionContext;
using usb_vault::card::vikey::VikeyCardMediaProbe;
using usb_vault::card::vikey::VikeyCardMediaState;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] VikeyCardMediaProbe probe(
    const VikeyCardMediaState media_state,
    const std::optional<CardRecordState> logical_state = std::nullopt) {
  return VikeyCardMediaProbe{
      .security_properties = 0x55AAu,
      .hardware_id = 0x12345678u,
      .media_state = media_state,
      .logical_state = logical_state,
  };
}

void require_invalid_probe(VikeyCardMediaProbe value,
                           const char* const message) {
  try {
    VikeyCardInsertionContext context(std::move(value));
    static_cast<void>(context);
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "invalid probe returned the wrong error");
    return;
  }
  throw std::runtime_error(message);
}

void test_new_media_routes_to_initialization_from_cached_probe() {
  for (const VikeyCardMediaState state : {
           VikeyCardMediaState::blank,
           VikeyCardMediaState::provisioning_interrupted,
       }) {
    VikeyCardInsertionContext context(probe(state));
    require(CardEntryRouter::route(context) ==
                CardEntryRoute::initialization_required,
            "eligible ViKey media did not route to initialization");
    require(context.probe().hardware_id == 0x12345678u &&
                context.probe().media_state == state &&
                !context.probe().logical_state.has_value(),
            "insertion context did not retain the atomic probe snapshot");
  }
}

void test_blocked_media_routes_to_unavailable() {
  for (const VikeyCardMediaState state : {
           VikeyCardMediaState::legacy_p1_or_foreign_prefix,
           VikeyCardMediaState::foreign_data,
           VikeyCardMediaState::conflicting_evidence,
       }) {
    VikeyCardInsertionContext context(probe(state));
    require(CardEntryRouter::route(context) == CardEntryRoute::unavailable,
            "blocked ViKey media reached a usable route");
  }
}

void test_product_states_route_without_another_card_read() {
  struct Case final {
    CardRecordState state;
    CardEntryRoute route;
  };
  for (const Case test_case : {
           Case{CardRecordState::provisioned_uninitialized,
                CardEntryRoute::initialization_required},
           Case{CardRecordState::initialized_v1,
                CardEntryRoute::login_required},
           Case{CardRecordState::unsupported_version,
                CardEntryRoute::unavailable},
           Case{CardRecordState::malformed,
                CardEntryRoute::unavailable},
       }) {
    VikeyCardInsertionContext context(
        probe(VikeyCardMediaState::product_evidence, test_case.state));
    require(CardEntryRouter::route(context) == test_case.route,
            "cached product state reached the wrong route");
    require(context.inspect_product_record_read_only() == test_case.state,
            "cached product state changed after routing");
  }
}

void test_inconsistent_or_unknown_probes_are_rejected() {
  auto zero_identity = probe(VikeyCardMediaState::blank);
  zero_identity.hardware_id = 0;
  require_invalid_probe(std::move(zero_identity),
                        "zero hardware identity was accepted");

  require_invalid_probe(
      probe(VikeyCardMediaState::product_evidence),
      "product evidence without logical state was accepted");
  require_invalid_probe(
      probe(VikeyCardMediaState::blank, CardRecordState::initialized_v1),
      "non-product media with logical state was accepted");
  require_invalid_probe(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::legacy_p1_blob),
      "legacy P1 state was accepted as a product record");
  require_invalid_probe(
      probe(static_cast<VikeyCardMediaState>(999)),
      "unknown media state was accepted");
  require_invalid_probe(
      probe(VikeyCardMediaState::product_evidence,
            static_cast<CardRecordState>(999)),
      "unknown product state was accepted");
}

void test_non_product_record_inspection_fails_closed() {
  VikeyCardInsertionContext context(probe(VikeyCardMediaState::blank));
  try {
    static_cast<void>(context.inspect_product_record_read_only());
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_operation),
            "non-product inspection returned the wrong error");
    return;
  }
  throw std::runtime_error("non-product context exposed a logical record");
}

void test_journal_evidence_preserves_legacy_rejection() {
  for (const auto state : {CardProductRecordState::initialized_v2,
                           CardProductRecordState::unsupported_version,
                           CardProductRecordState::malformed}) {
    VikeyCardInsertionContext context(
        probe(VikeyCardMediaState::product_evidence,
              CardRecordState::unsupported_version), state);
    for (int repeat = 0; repeat < 2; ++repeat) {
      CardProductRecordState routed_state = CardProductRecordState::malformed;
      require(CardEntryRouter::route(context, nullptr, &routed_state) ==
                  (state == CardProductRecordState::initialized_v2
                       ? CardEntryRoute::login_required
                       : CardEntryRoute::unavailable) && routed_state == state,
              "cached journal evidence reached the wrong route or format");
      require(context.inspect_product_record_read_only() ==
                  CardRecordState::unsupported_version &&
                  context.probe().logical_state ==
                      CardRecordState::unsupported_version &&
                  context.probe().hardware_id == 0x12345678u &&
                  context.probe().security_properties == 0x55AAu,
              "journal evidence changed the original legacy probe");
    }
  }
}

void require_invalid_journal_evidence(VikeyCardMediaProbe value,
                                      const CardProductRecordState state) {
  try {
    VikeyCardInsertionContext context(std::move(value), state);
    static_cast<void>(context);
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "invalid journal evidence returned the wrong error");
    return;
  }
  throw std::runtime_error("inconsistent journal evidence was accepted");
}

void test_journal_evidence_cannot_promote_other_media_or_formats() {
  for (const auto state : {CardProductRecordState::provisioned_uninitialized,
                           CardProductRecordState::initialized_v1,
                           CardProductRecordState::legacy_p1_blob,
                           static_cast<CardProductRecordState>(999)}) {
    require_invalid_journal_evidence(
        probe(VikeyCardMediaState::product_evidence,
              CardRecordState::unsupported_version), state);
  }
  for (const auto media : {VikeyCardMediaState::blank,
                           VikeyCardMediaState::provisioning_interrupted,
                           VikeyCardMediaState::foreign_data,
                           VikeyCardMediaState::legacy_p1_or_foreign_prefix,
                           VikeyCardMediaState::conflicting_evidence}) {
    require_invalid_journal_evidence(probe(media),
                                    CardProductRecordState::initialized_v2);
  }
  for (const auto state : {CardRecordState::provisioned_uninitialized,
                           CardRecordState::initialized_v1,
                           CardRecordState::malformed}) {
    require_invalid_journal_evidence(
        probe(VikeyCardMediaState::product_evidence, state),
        CardProductRecordState::initialized_v2);
  }
  auto no_identity = probe(VikeyCardMediaState::product_evidence,
                           CardRecordState::unsupported_version);
  no_identity.hardware_id = 0;
  require_invalid_journal_evidence(no_identity,
                                  CardProductRecordState::initialized_v2);
}

}  // namespace

int main() {
  try {
    test_new_media_routes_to_initialization_from_cached_probe();
    test_blocked_media_routes_to_unavailable();
    test_product_states_route_without_another_card_read();
    test_inconsistent_or_unknown_probes_are_rejected();
    test_non_product_record_inspection_fails_closed();
    test_journal_evidence_preserves_legacy_rejection();
    test_journal_evidence_cannot_promote_other_media_or_formats();
    std::cout << "vikey_card_insertion_context_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_card_insertion_context_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
