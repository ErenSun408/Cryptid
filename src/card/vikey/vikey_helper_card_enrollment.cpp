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

void VikeyHelperCardEnrollment::bootstrap(
    const VikeyCardMediaProbe& context) {
  if (context.hardware_id == 0 ||
      !eligible_bootstrap_state(context.media_state)) {
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

}  // namespace usb_vault::card::vikey
