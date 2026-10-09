#include "card/diagnostic_error.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "card/vikey/card_operation_lease.h"
#include "card/capture_failure.h"
#include "card/vikey/vikey_helper_card_enrollment.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v2.h"

#include <algorithm>
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

class VikeyHelperCardResetTarget final : public ICardResetTarget {
 public:
  VikeyHelperCardResetTarget(VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& provider, const VikeyCardMediaProbe context)
      : config_(std::move(config)), provider_(provider),
        hardware_id_(validate_login_context(context).hardware_id) {}
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
          credentials.api_key, credentials.user_pin, expected, config_.timeout, true);
    } catch (...) { USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_enrollment", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); rethrow_enrollment_failure(); }
  }
 private:
  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& provider_;
  std::uint32_t hardware_id_;
};

class JournalLoginTarget final : public ICardLoginTarget {
 public:
  explicit JournalLoginTarget(std::unique_ptr<IVikeyCardJournalTransport> transport)
      : transport_(std::move(transport)), target_(*transport_) {}
  CardProductRecordState record_format() const noexcept override {
    return target_.record_format();
  }
  security::SecureBuffer read_record() override { return target_.read_record(); }
  void complete_authenticated_read(std::span<const std::byte> expected,
      const security::SecureBuffer& root) override {
    target_.complete_authenticated_read(expected, root);
  }
 private:
  std::unique_ptr<IVikeyCardJournalTransport> transport_;
  VikeyCardJournalLoginTarget target_;
};

class JournalInitializationTarget final : public ICardInitializationTarget {
 public:
  JournalInitializationTarget(VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& provider, IVikeyCardEnrollmentIo* io,
      VikeyCardMediaProbe probe, std::unique_ptr<IVikeyCardJournalTransport> transport)
      : enrollment_(std::move(config), provider, io), probe_(probe),
        transport_(std::move(transport)), store_(*transport_) {}
  security::SecureBuffer initialize_confirmed(security::ConfirmedPassword password) override {
    CardOperationLease lease;
    if (attempted_) throw std::system_error(make_error_code(CardError::invalid_operation));
    attempted_ = true; // Uncertain writes require a fresh insertion, never a new root retry.
    if (eligible_bootstrap_state(probe_.media_state)) enrollment_.bootstrap(probe_);
    auto marker = store_.snapshot_provisioning();
    auto plan = store_.prepare_initialization(marker.bytes(), std::move(password));
    return store_.initialize(plan);
  }
 private:
  VikeyHelperCardEnrollment enrollment_;
  VikeyCardMediaProbe probe_;
  std::unique_ptr<IVikeyCardJournalTransport> transport_;
  VikeyCardJournalStore store_;
  bool attempted_{};
};

class JournalUpdateTarget final : public ICardUpdateTarget {
 public:
  JournalUpdateTarget(std::unique_ptr<IVikeyCardJournalTransport> transport,
      CardProductRecordState format)
      : transport_(std::move(transport)), store_(*transport_), format_(format) {}
  CardProductRecordState record_format() const noexcept override { return format_; }
  void append_password(security::SecureBuffer existing, security::SecureBuffer password,
      const std::function<bool()>& confirm) override {
    CardOperationLease lease;
    if (attempted_ || existing.empty() || password.empty() || !confirm)
      throw std::system_error(make_error_code(CardError::invalid_operation));
    attempted_ = true;
    if (std::ranges::equal(existing.bytes(), password.bytes()))
      throw std::invalid_argument("new password must differ from current password");
    const auto duplicate = [](const security::SecureBuffer& value) {
      security::SecureBuffer result(value.size());
      std::ranges::copy(value.bytes(), result.bytes().begin());
      return result;
    };
    security::SecureBuffer record, root;
    if (format_ == CardProductRecordState::initialized_v1) {
      record = store_.snapshot_legacy();
      root = unlock_card_record_v1(record.bytes(), duplicate(existing));
      auto upgrade = store_.prepare_upgrade(record.bytes(), duplicate(existing));
      if (!confirm()) throw std::system_error(make_error_code(CardError::invalid_operation));
      store_.commit_upgrade(std::move(upgrade));
      record = store_.snapshot();
    } else {
      record = store_.snapshot();
      root = unlock_card_record_v2(record.bytes(), duplicate(existing));
    }
    auto replacement = append_card_password_v2(record.bytes(), std::move(existing), std::move(password));
    if (format_ == CardProductRecordState::initialized_v2 && !confirm())
      throw std::system_error(make_error_code(CardError::invalid_operation));
    store_.commit(record.bytes(), replacement.bytes(), root);
  }
 private:
  std::unique_ptr<IVikeyCardJournalTransport> transport_;
  VikeyCardJournalStore store_;
  CardProductRecordState format_;
  bool attempted_{};
};

}  // namespace

VikeyHelperCardEnrollment::VikeyHelperCardEnrollment(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider, IVikeyCardEnrollmentIo* io)
    : config_(std::move(config)),
      credential_provider_(credential_provider), io_(io) {}

VikeyCardMediaProbe VikeyHelperCardEnrollment::probe() {
  try {
    if (io_) return io_->probe();
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
  insertion_identity_.reset(); // A failed replacement inspection also expires the old context.
  CardOperationLease lease;
  auto candidate = std::make_unique<VikeyCardInsertionContext>(probe());
  const auto& observed = candidate->probe();
  if (observed.media_state == VikeyCardMediaState::product_evidence &&
      (observed.logical_state == CardRecordState::unsupported_version ||
       observed.logical_state == CardRecordState::provisioned_uninitialized)) {
    auto bound_transport = transport(observed.hardware_id);
    VikeyCardJournalStore store(*bound_transport);
    const auto state = store.inspect_entry();
    if (observed.logical_state == CardRecordState::unsupported_version) {
      candidate = std::make_unique<VikeyCardInsertionContext>(observed, state);
    } else if (state != CardProductRecordState::provisioned_uninitialized) {
      throw std::system_error(make_error_code(CardError::invalid_configuration));
    }
  }
  insertion_identity_ = std::make_shared<const std::byte>();
  candidate->insertion_identity_ = insertion_identity_;
  return candidate;
}

std::unique_ptr<IVikeyCardJournalTransport> VikeyHelperCardEnrollment::transport(
    const std::uint32_t hardware_id) {
  auto result = io_ ? io_->transport(hardware_id)
      : std::make_unique<VikeyHelperCardJournalTransport>(config_, credential_provider_, hardware_id);
  if (!result) throw std::system_error(make_error_code(CardError::backend_unavailable));
  return result;
}

const VikeyCardInsertionContext& VikeyHelperCardEnrollment::require_context(
    const application::IReadOnlyCardEntryInspection& context) const {
  const auto* insertion = dynamic_cast<const VikeyCardInsertionContext*>(&context);
  if (!insertion || !insertion_identity_ || insertion->insertion_identity_ != insertion_identity_) {
    throw std::system_error(make_error_code(CardError::invalid_configuration),
        "ViKey operation requires its current insertion context");
  }
  return *insertion;
}

std::unique_ptr<application::IReadOnlyCardEntryInspection>
VikeyHelperCardEnrollment::inspect_inserted_card() {
  return inspect_entry();
}

std::unique_ptr<ICardInitializationTarget>
VikeyHelperCardEnrollment::make_initialization_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto& insertion = require_context(context);
  if (!eligible_bootstrap_state(insertion.probe().media_state) &&
      !verified_provisioning_marker(insertion.probe()) &&
      insertion.journal_state_ != CardProductRecordState::provisioned_uninitialized) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey initialization requires its original insertion context");
  }
  return std::make_unique<JournalInitializationTarget>(
      config_, credential_provider_, io_, insertion.probe(), transport(insertion.probe().hardware_id));
}

std::unique_ptr<ICardLoginTarget>
VikeyHelperCardEnrollment::make_login_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto& insertion = require_context(context);
  if (insertion.journal_state_ == CardProductRecordState::initialized_v2) {
    return std::make_unique<JournalLoginTarget>(transport(insertion.probe().hardware_id));
  }
  return std::make_unique<VikeyHelperCardLoginTarget>(
      config_, credential_provider_, insertion.probe());
}

std::unique_ptr<ICardUpdateTarget>
VikeyHelperCardEnrollment::make_update_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto& insertion = require_context(context);
  return std::make_unique<VikeyHelperCardUpdateTarget>(config_, credential_provider_, insertion.probe());
}

std::unique_ptr<ICardUpdateTarget>
VikeyHelperCardEnrollment::make_password_update_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto& insertion = require_context(context);
  const auto format = insertion.journal_state_.value_or(
      product_state_from_legacy(insertion.probe().logical_state.value_or(CardRecordState::malformed)));
  if (format != CardProductRecordState::initialized_v1 && format != CardProductRecordState::initialized_v2)
    throw std::system_error(make_error_code(CardError::invalid_configuration));
  return std::make_unique<JournalUpdateTarget>(transport(insertion.probe().hardware_id), format);
}

std::unique_ptr<application::ICardOperationLease>
VikeyHelperCardEnrollment::acquire_operation() {
  return std::make_unique<CardOperationLease>();
}

std::unique_ptr<ICardResetTarget> VikeyHelperCardEnrollment::make_reset_target(
    const application::IReadOnlyCardEntryInspection& context) {
  const auto& insertion = require_context(context);
  return std::make_unique<VikeyHelperCardResetTarget>(config_, credential_provider_, insertion.probe());
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
    if (io_) { io_->bootstrap(context); return; }
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    bootstrap_vikey_development_card_media_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        context.hardware_id, context.media_state, credentials.user_sn,
        credentials.api_key, credentials.user_pin, config_.timeout);
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
