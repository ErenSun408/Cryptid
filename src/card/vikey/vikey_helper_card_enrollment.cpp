#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/card/card_error.h"

#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void rethrow_enrollment_failure() {
  try {
    throw;
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) throw;
  } catch (...) {
  }
  throw std::system_error(
      make_error_code(CardError::backend_unavailable),
      "ViKey credential provider or enrollment helper failed");
}

[[nodiscard]] bool eligible_bootstrap_state(
    const VikeyCardMediaState state) noexcept {
  return state == VikeyCardMediaState::blank ||
         state == VikeyCardMediaState::provisioning_interrupted;
}

[[nodiscard]] VikeyCardMediaProbe validate_initialization_context(
    const VikeyCardMediaProbe context) {
  if (context.hardware_id == 0 ||
      !eligible_bootstrap_state(context.media_state) ||
      context.logical_state.has_value()) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey initialization target requires eligible insertion context");
  }
  return context;
}

}  // namespace

VikeyHelperCardEnrollment::VikeyHelperCardEnrollment(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider)
    : config_(std::move(config)),
      credential_provider_(credential_provider) {}

VikeyCardMediaProbe VikeyHelperCardEnrollment::probe() {
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    return probe_vikey_development_card_media_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        credentials.user_sn, credentials.api_key, credentials.user_pin,
        config_.timeout);
  } catch (...) {
    rethrow_enrollment_failure();
  }
}

std::unique_ptr<VikeyCardInsertionContext>
VikeyHelperCardEnrollment::inspect_entry() {
  return std::make_unique<VikeyCardInsertionContext>(probe());
}

std::unique_ptr<application::IReadOnlyCardEntryInspection>
VikeyHelperCardEnrollment::inspect_inserted_card() {
  return inspect_entry();
}

void VikeyHelperCardEnrollment::bootstrap(
    const VikeyCardMediaProbe& context) {
  if (context.hardware_id == 0 ||
      !eligible_bootstrap_state(context.media_state) ||
      context.logical_state.has_value()) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey enrollment context is not eligible for bootstrap");
  }
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    bootstrap_vikey_development_card_media_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        context.hardware_id, context.media_state, credentials.user_sn,
        credentials.api_key, credentials.user_pin, config_.timeout);
  } catch (...) {
    rethrow_enrollment_failure();
  }
}

VikeyHelperCardInitializationTarget::VikeyHelperCardInitializationTarget(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider,
    const VikeyCardMediaProbe context)
    : context_(validate_initialization_context(context)),
      enrollment_(config, credential_provider),
      backend_(std::move(config), credential_provider,
               context_.hardware_id),
      store_(backend_) {}

void VikeyHelperCardInitializationTarget::ensure_provisioning_record() {
  if (provisioning_state_ == ProvisioningState::verified) return;
  if (provisioning_state_ == ProvisioningState::failed) {
    throw std::system_error(
        make_error_code(CardError::invalid_operation),
        "ViKey initialization target requires a fresh insertion probe");
  }
  provisioning_state_ = ProvisioningState::failed;
  enrollment_.bootstrap(context_);
  provisioning_state_ = ProvisioningState::verified;
}

ICardStore& VikeyHelperCardInitializationTarget::card_store() noexcept {
  return store_;
}

}  // namespace usb_vault::card::vikey
