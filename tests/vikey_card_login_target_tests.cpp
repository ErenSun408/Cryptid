#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/card/card_error.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardLoginTarget;
using usb_vault::card::vikey::IVikeyCredentialProvider;
using usb_vault::card::vikey::VikeyCardInsertionContext;
using usb_vault::card::vikey::VikeyCardMediaProbe;
using usb_vault::card::vikey::VikeyCardMediaState;
using usb_vault::card::vikey::VikeyCredentialSet;
using usb_vault::card::vikey::VikeyHelperBackendConfig;
using usb_vault::card::vikey::VikeyHelperCardEnrollment;
using usb_vault::card::vikey::VikeyHelperCardLoginTarget;

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
    return usb_vault::application::CardEntryMediaEvidence::product_record;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    return CardRecordState::initialized_v1;
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
    VikeyHelperCardLoginTarget target(
        VikeyHelperBackendConfig{}, provider, std::move(context));
    static_cast<void>(target);
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "invalid login target returned the wrong error");
    require(provider.loads == 0,
            "invalid login target loaded credentials");
    return;
  }
  throw std::runtime_error(message);
}

void test_exact_initialized_context_is_side_effect_free() {
  RejectingCredentialProvider provider;
  VikeyHelperCardLoginTarget target(
      VikeyHelperBackendConfig{}, provider,
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::initialized_v1));

  require(provider.loads == 0,
          "login target construction loaded credentials");
}

void test_enrollment_factory_accepts_only_its_initialized_context() {
  RejectingCredentialProvider provider;
  VikeyHelperCardEnrollment enrollment(VikeyHelperBackendConfig{}, provider);
  VikeyCardInsertionContext insertion(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::initialized_v1));

  std::unique_ptr<ICardLoginTarget> target =
      enrollment.make_login_target(insertion);
  require(target != nullptr && provider.loads == 0,
          "login factory did not create a side-effect-free target");

  ForeignInspection foreign;
  try {
    static_cast<void>(enrollment.make_login_target(foreign));
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(
                                CardError::invalid_configuration),
            "foreign login context returned the wrong error");
    require(provider.loads == 0,
            "foreign login context loaded credentials");
    return;
  }
  throw std::runtime_error("foreign login context was accepted");
}

void test_all_other_contexts_are_rejected() {
  auto zero_id = probe(VikeyCardMediaState::product_evidence,
                       CardRecordState::initialized_v1);
  zero_id.hardware_id = 0;
  require_invalid_target(std::move(zero_id),
                         "zero hardware identity was accepted");

  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence),
      "product evidence without logical state was accepted");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::provisioned_uninitialized),
      "provisioning record was accepted for login");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::unsupported_version),
      "unsupported record was accepted for login");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            CardRecordState::malformed),
      "malformed record was accepted for login");
  require_invalid_target(probe(VikeyCardMediaState::blank),
                         "blank media was accepted for login");
  require_invalid_target(
      probe(VikeyCardMediaState::legacy_p1_or_foreign_prefix),
      "legacy or foreign prefix was accepted for login");
  require_invalid_target(probe(VikeyCardMediaState::foreign_data),
                         "foreign data was accepted for login");
  require_invalid_target(probe(VikeyCardMediaState::conflicting_evidence),
                         "conflicting evidence was accepted for login");
  require_invalid_target(
      probe(static_cast<VikeyCardMediaState>(999)),
      "unknown media state was accepted for login");
  require_invalid_target(
      probe(VikeyCardMediaState::product_evidence,
            static_cast<CardRecordState>(999)),
      "unknown logical state was accepted for login");
}

void test_probe_handoff_is_one_shot_and_does_not_authenticate() {
  RejectingCredentialProvider provider;
  VikeyHelperCardEnrollment enrollment(VikeyHelperBackendConfig{}, provider);
  enrollment.prime_login_inspection(probe(VikeyCardMediaState::product_evidence,CardRecordState::initialized_v1));
  auto insertion = enrollment.inspect_entry();
  require(insertion && insertion->inspect_product_record_read_only()==CardRecordState::initialized_v1 && provider.loads==0,
          "probe handoff repeated hardware inspection");
  auto login = enrollment.make_login_target(*insertion);
  require(login && provider.loads==0,"probe handoff did not preserve normal login target");
  bool consumed=false;
  try { static_cast<void>(enrollment.inspect_entry()); } catch (...) { consumed=true; }
  require(consumed && provider.loads==1,"one-shot probe was reused for another insertion");
  bool rejected=false;
  try { enrollment.prime_login_inspection(probe(VikeyCardMediaState::blank)); } catch (...) { rejected=true; }
  require(rejected,"blank card accepted as login-ready handoff");
}

}  // namespace

int main() {
  try {
    test_exact_initialized_context_is_side_effect_free();
    test_probe_handoff_is_one_shot_and_does_not_authenticate();
    test_enrollment_factory_accepts_only_its_initialized_context();
    test_all_other_contexts_are_rejected();
    std::cout << "vikey_card_login_target_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "vikey_card_login_target_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
