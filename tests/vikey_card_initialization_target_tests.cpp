#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/card/card_error.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::vikey::IVikeyCredentialProvider;
using usb_vault::card::vikey::VikeyCardMediaProbe;
using usb_vault::card::vikey::VikeyCardMediaState;
using usb_vault::card::vikey::VikeyCredentialSet;
using usb_vault::card::vikey::VikeyHelperBackendConfig;
using usb_vault::card::vikey::VikeyHelperCardEnrollment;
using usb_vault::card::vikey::VikeyHelperCardInitializationTarget;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class RejectingCredentialProvider final : public IVikeyCredentialProvider {
 public:
  [[nodiscard]] VikeyCredentialSet load_credentials() override {
    ++loads;
    throw std::runtime_error("credentials must not be loaded");
  }

  int loads{};
};

class ForeignInspection final
    : public usb_vault::application::IReadOnlyCardEntryInspection {
 public:
  [[nodiscard]] usb_vault::application::CardEntryMediaEvidence
  inspect_media() override {
    return usb_vault::application::CardEntryMediaEvidence::initializable_media;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    return CardRecordState::malformed;
  }
};

[[nodiscard]] VikeyCardMediaProbe probe(
    const VikeyCardMediaState media,
    const std::optional<CardRecordState> logical = std::nullopt) {
  return VikeyCardMediaProbe{
      .security_properties = 0x55AAu,
      .hardware_id = 0x12345678u,
      .media_state = media,
      .logical_state = logical,
  };
}

void require_invalid_target(VikeyCardMediaProbe context,
                            const char* const message) {
  RejectingCredentialProvider provider;
  try {
    VikeyHelperCardInitializationTarget target(
        VikeyHelperBackendConfig{}, provider, std::move(context));
    static_cast<void>(target);
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "invalid target returned the wrong error");
    require(provider.loads == 0,
            "invalid target loaded vendor credentials");
    return;
  }
  throw std::runtime_error(message);
}

void test_verified_marker_skips_bootstrap_and_credentials() {
  RejectingCredentialProvider provider;
  VikeyHelperCardInitializationTarget target(
      VikeyHelperBackendConfig{}, provider,
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::provisioned_uninitialized));

  target.ensure_provisioning_record();
  target.ensure_provisioning_record();
  require(provider.loads == 0,
          "verified provisioning marker started a helper operation");
  require(!target.card_store().capabilities().supports(
              usb_vault::card::CardCapability::multiple_pins),
          "marker target exposed unexpected card capabilities");
}

void test_bare_initializable_contexts_remain_constructible() {
  for (const VikeyCardMediaState media : {
           VikeyCardMediaState::blank,
           VikeyCardMediaState::provisioning_interrupted,
       }) {
    RejectingCredentialProvider provider;
    VikeyHelperCardInitializationTarget target(
        VikeyHelperBackendConfig{}, provider, probe(media));
    require(provider.loads == 0,
            "bare target loaded credentials during construction");
    static_cast<void>(target.card_store());
  }
}

void test_enrollment_factory_accepts_only_its_concrete_context() {
  RejectingCredentialProvider provider;
  VikeyHelperCardEnrollment enrollment(VikeyHelperBackendConfig{}, provider);
  usb_vault::card::vikey::VikeyCardInsertionContext insertion(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::provisioned_uninitialized));

  std::unique_ptr<usb_vault::card::ICardInitializationTarget> target =
      enrollment.make_initialization_target(insertion);
  require(target != nullptr && provider.loads == 0,
          "factory did not create a side-effect-free bound target");

  ForeignInspection foreign;
  try {
    static_cast<void>(enrollment.make_initialization_target(foreign));
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "foreign context returned the wrong factory error");
    require(provider.loads == 0,
            "foreign context loaded vendor credentials");
    return;
  }
  throw std::runtime_error("foreign insertion context was accepted");
}

void test_all_other_contexts_are_rejected() {
  auto zero_id = probe(VikeyCardMediaState::blank);
  zero_id.hardware_id = 0;
  require_invalid_target(std::move(zero_id),
                         "zero hardware identity was accepted");

  require_invalid_target(
      probe(VikeyCardMediaState::blank, CardRecordState::initialized_v1),
      "bare media carrying logical state was accepted");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence),
      "product evidence without logical state was accepted");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::initialized_v1),
      "initialized product record was accepted for initialization");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::unsupported_version),
      "unsupported product record was accepted for initialization");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::malformed),
      "malformed product record was accepted for initialization");
  require_invalid_target(
      probe(VikeyCardMediaState::legacy_p1_or_foreign_prefix),
      "legacy or same-shaped foreign prefix was accepted");
  require_invalid_target(probe(VikeyCardMediaState::foreign_data),
                         "foreign data was accepted");
  require_invalid_target(probe(VikeyCardMediaState::conflicting_evidence),
                         "conflicting evidence was accepted");
  require_invalid_target(
      probe(static_cast<VikeyCardMediaState>(999)),
      "unknown media state was accepted");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            static_cast<CardRecordState>(999)),
      "unknown logical state was accepted");
}

}  // namespace

int main() {
  try {
    test_verified_marker_skips_bootstrap_and_credentials();
    test_bare_initializable_contexts_remain_constructible();
    test_enrollment_factory_accepts_only_its_concrete_context();
    test_all_other_contexts_are_rejected();
    std::cout << "vikey_card_initialization_target_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_card_initialization_target_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
