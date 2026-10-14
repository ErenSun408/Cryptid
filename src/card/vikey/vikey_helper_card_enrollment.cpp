#include "card/diagnostic_error.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "card/vikey/card_operation_lease.h"
#include "card/capture_failure.h"
#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/card/card_error.h"

#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void rethrow_enrollment_failure() {
  const auto original = std::current_exception();
  try {
    throw;
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) throw;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
  throw CapturedFailureError(capture_failure(original));
}

[[nodiscard]] bool eligible_bootstrap_state(
    const VikeyCardMediaState state) noexcept {
  return state == VikeyCardMediaState::blank ||
         state == VikeyCardMediaState::provisioning_interrupted;
}

[[nodiscard]] bool verified_provisioning_marker(
    const VikeyCardMediaProbe& context) noexcept {
  return context.media_state == VikeyCardMediaState::product_evidence &&
         context.logical_state == CardRecordState::provisioned_uninitialized;
}

[[nodiscard]] VikeyCardMediaProbe validate_initialization_context(
    const VikeyCardMediaProbe context) {
  const bool eligible_bare_media =
      eligible_bootstrap_state(context.media_state) &&
      !context.logical_state.has_value();
  if (context.hardware_id == 0 ||
      (!eligible_bare_media && !verified_provisioning_marker(context))) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey initialization target requires eligible insertion context");
  }
  return context;
}

[[nodiscard]] VikeyCardMediaProbe validate_login_context(
    const VikeyCardMediaProbe context) {
  if (context.hardware_id == 0 ||
      context.media_state != VikeyCardMediaState::product_evidence ||
      context.logical_state != CardRecordState::initialized_v1) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey login target requires initialized insertion context");
  }
  return context;
}

// Reset also accepts a future-version record (the withdrawn P5.4 format):
// such a card can only be reset and initialized again.
[[nodiscard]] VikeyCardMediaProbe validate_reset_context(
    const VikeyCardMediaProbe context) {
  if (context.hardware_id == 0 ||
      context.media_state != VikeyCardMediaState::product_evidence ||
      (context.logical_state != CardRecordState::initialized_v1 &&
       context.logical_state != CardRecordState::unsupported_version)) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey reset target requires an initialized or earlier-format card");
  }
  return context;
}

class VikeyHelperCardResetTarget final : public ICardResetTarget {
 public:
  VikeyHelperCardResetTarget(VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& provider, const VikeyCardMediaProbe context)
      : config_(std::move(config)), provider_(provider),
        hardware_id_(validate_reset_context(context).hardware_id) {}
  security::SecureBuffer snapshot() override {
    try {
      auto credentials = provider_.load_credentials();
      return read_vikey_development_card_record_with_network_access(
          config_.helper_path, config_.helper_digest, config_.vendor_directory,
          config_.vendor_digest, hardware_id_, credentials.user_sn,
          credentials.api_key, credentials.user_pin, config_.timeout, true);
    } catch (...) { USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); rethrow_enrollment_failure(); }
  }
  void reset(const security::SecureBuffer& expected) override {
    try {
      auto credentials = provider_.load_credentials();
      write_vikey_development_card_record_with_network_access(
          config_.helper_path, config_.helper_digest, config_.vendor_directory,
          config_.vendor_digest, hardware_id_, credentials.user_sn,
          credentials.api_key, credentials.user_pin, expected, config_.write_timeout, true);
    } catch (...) { USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); rethrow_enrollment_failure(); }
  }
 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& provider_;
  std::uint32_t hardware_id_;
};

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
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    rethrow_enrollment_failure();
  }
}

std::unique_ptr<VikeyCardInsertionContext>
VikeyHelperCardEnrollment::inspect_entry() {
  if (primed_login_probe_) {
    const auto value = *primed_login_probe_;
    primed_login_probe_.reset();
    return std::make_unique<VikeyCardInsertionContext>(value);
  }
  return std::make_unique<VikeyCardInsertionContext>(probe());
}
void VikeyHelperCardEnrollment::prime_login_inspection(const VikeyCardMediaProbe& value) {
  primed_login_probe_.reset();
  primed_login_probe_ = validate_login_context(value);
}

std::unique_ptr<application::IReadOnlyCardEntryInspection>
VikeyHelperCardEnrollment::inspect_inserted_card() {
  return inspect_entry();
}

std::unique_ptr<ICardInitializationTarget>
VikeyHelperCardEnrollment::make_initialization_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto* const insertion =
      dynamic_cast<const VikeyCardInsertionContext*>(&context);
  if (insertion == nullptr) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey initialization requires its original insertion context");
  }
  return std::make_unique<VikeyHelperCardInitializationTarget>(
      config_, credential_provider_, insertion->probe());
}

std::unique_ptr<ICardLoginTarget>
VikeyHelperCardEnrollment::make_login_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto* const insertion =
      dynamic_cast<const VikeyCardInsertionContext*>(&context);
  if (insertion == nullptr) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey login requires its original insertion context");
  }
  return std::make_unique<VikeyHelperCardLoginTarget>(
      config_, credential_provider_, insertion->probe());
}

std::unique_ptr<ICardUpdateTarget>
VikeyHelperCardEnrollment::make_update_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto* const insertion =
      dynamic_cast<const VikeyCardInsertionContext*>(&context);
  if (insertion == nullptr) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey record update requires its original insertion context");
  }
  return std::make_unique<VikeyHelperCardUpdateTarget>(
      config_, credential_provider_, insertion->probe());
}

std::unique_ptr<application::ICardOperationLease>
VikeyHelperCardEnrollment::acquire_operation() {
  return std::make_unique<CardOperationLease>();
}

std::unique_ptr<ICardResetTarget> VikeyHelperCardEnrollment::make_reset_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto* insertion = dynamic_cast<const VikeyCardInsertionContext*>(&context);
  if (!insertion)
    throw std::system_error(make_error_code(CardError::invalid_configuration));
  return std::make_unique<VikeyHelperCardResetTarget>(config_, credential_provider_, insertion->probe());
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
        credentials.api_key, credentials.user_pin, config_.write_timeout);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      store_(backend_) {
  if (verified_provisioning_marker(context_)) {
    provisioning_state_ = ProvisioningState::verified;
  }
}

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

VikeyHelperCardLoginTarget::VikeyHelperCardLoginTarget(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider,
    const VikeyCardMediaProbe context)
    : context_(validate_login_context(context)),
      backend_(std::move(config), credential_provider,
               context_.hardware_id),
      store_(backend_) {}

security::SecureBuffer VikeyHelperCardLoginTarget::read_record() {
  return store_.get_blob();
}

VikeyHelperCardUpdateTarget::VikeyHelperCardUpdateTarget(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider,
    const VikeyCardMediaProbe context)
    : context_(validate_login_context(context)),
      backend_(std::move(config), credential_provider,
               context_.hardware_id),
      store_(backend_) {}

ICardStore& VikeyHelperCardUpdateTarget::card_store() noexcept {
  return store_;
}

}  // namespace usb_vault::card::vikey
