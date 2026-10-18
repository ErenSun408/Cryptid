#include "card/vikey/vikey_card_backup.h"
#include "card/vikey/card_recheck_coordinator.h"
#include "card/diagnostic_error.h"
#include "usb_vault/contacts/contact_store_v1.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include <algorithm>
#include <system_error>

namespace usb_vault::card::vikey {
namespace {
[[noreturn]] void reject() {
  throw std::system_error(make_error_code(CardError::invalid_operation),
                          "spare card operation rejected");
}
bool initialized(const VikeyCardMediaProbe& p) {
  return p.hardware_id != 0 && p.media_state == VikeyCardMediaState::product_evidence &&
         p.logical_state == CardRecordState::initialized_v1;
}
}
VikeyHelperCardBackupBackend::VikeyHelperCardBackupBackend(
    VikeyHelperBackendConfig config, IVikeyCredentialProvider& credentials)
    : config_(std::move(config)), credentials_(credentials) {}
VikeyCardMediaProbe VikeyHelperCardBackupBackend::probe() {
  auto c = credentials_.load_credentials();
  return probe_vikey_development_card_media_with_network_access(
      config_.helper_path, config_.helper_digest, config_.vendor_directory,
      config_.vendor_digest, c.user_sn, c.api_key, c.user_pin, config_.timeout);
}
security::SecureBuffer VikeyHelperCardBackupBackend::snapshot(std::uint32_t identity) {
  auto c = credentials_.load_credentials();
  return read_vikey_development_card_record_with_network_access(
      config_.helper_path, config_.helper_digest, config_.vendor_directory,
      config_.vendor_digest, identity, c.user_sn, c.api_key, c.user_pin,
      config_.timeout, false, true);
}
std::optional<security::SecureBuffer>
VikeyHelperCardBackupBackend::contact_region(std::uint32_t identity) {
  auto c = credentials_.load_credentials();
  try {
    return read_vikey_development_contact_region_with_network_access(
        config_.helper_path, config_.helper_digest, config_.vendor_directory,
        config_.vendor_digest, identity, c.user_sn, c.api_key, c.user_pin,
        config_.timeout);
  } catch (const std::system_error& error) {
    if (error.code() == make_error_code(CardError::invalid_configuration) &&
        diagnostic_stage(error) ==
            static_cast<std::uint32_t>(DiagnosticStage::sdk_capacity))
      return std::nullopt;
    throw;
  }
}
void VikeyHelperCardBackupBackend::write(std::uint32_t identity,
                                        const security::SecureBuffer& record,
                                        const security::SecureBuffer* contact_region) {
  auto c = credentials_.load_credentials();
  if (contact_region == nullptr) {
    write_vikey_development_card_record_with_network_access(
        config_.helper_path, config_.helper_digest, config_.vendor_directory,
        config_.vendor_digest, identity, c.user_sn, c.api_key, c.user_pin,
        record, config_.write_timeout, false, true);
    return;
  }
  security::SecureBuffer payload(record.size() + contact_region->size());
  std::ranges::copy(record.bytes(), payload.bytes().begin());
  std::ranges::copy(contact_region->bytes(),
                    payload.bytes().begin() + static_cast<std::ptrdiff_t>(record.size()));
  write_vikey_development_card_record_with_network_access(
      config_.helper_path, config_.helper_digest, config_.vendor_directory,
      config_.vendor_digest, identity, c.user_sn, c.api_key, c.user_pin,
      payload, config_.write_timeout, false, true);
}
void VikeyCardBackup::cancel() noexcept {
  record_.clear(); contacts_.reset(); source_ = 0; deadline_ = {};
}
bool VikeyCardBackup::pending(Clock::time_point now) noexcept {
  if (record_.size() == 0 || now >= deadline_) { cancel(); return false; }
  return true;
}
unsigned int VikeyCardBackup::remaining_seconds(Clock::time_point now) noexcept {
  if (!pending(now)) return 0;
  return static_cast<unsigned int>(std::chrono::ceil<std::chrono::seconds>(deadline_ - now).count());
}
void VikeyCardBackup::prepare(Clock::time_point now) {
  cancel();
  try {
    ForegroundCardOperationLease lease;
    const auto source = backend_.probe();
    if (!initialized(source)) reject();
    auto first = backend_.snapshot(source.hardware_id);
    auto contacts = backend_.contact_region(source.hardware_id);
    auto second = backend_.snapshot(source.hardware_id);
    auto contacts_again = backend_.contact_region(source.hardware_id);
    const auto confirmed = backend_.probe();
    if (!initialized(confirmed) || confirmed.hardware_id != source.hardware_id ||
        inspect_card_record(first.bytes()) != CardRecordState::initialized_v1 ||
        !std::ranges::equal(first.bytes(), second.bytes()) ||
        contacts.has_value() != contacts_again.has_value() ||
        (contacts && (contacts->size() != contacts::kContactStoreV1RegionSize ||
                      !std::ranges::equal(contacts->bytes(), contacts_again->bytes()))))
      reject();
    record_ = std::move(first);
    contacts_ = std::move(contacts);
    source_ = source.hardware_id;
    deadline_ = now + std::chrono::minutes(3);
  } catch (...) { cancel(); throw; }
}
bool VikeyCardBackup::target_eligible(const VikeyCardMediaProbe& target, Clock::time_point now) noexcept {
  return pending(now) && target.hardware_id != 0 && target.hardware_id != source_ &&
      ((target.media_state == VikeyCardMediaState::blank && !target.logical_state) ||
       (target.media_state == VikeyCardMediaState::product_evidence &&
        target.logical_state == CardRecordState::provisioned_uninitialized));
}
void VikeyCardBackup::complete(Clock::time_point now, std::uint32_t expected_target) {
  try {
    if (!pending(now)) reject();
    ForegroundCardOperationLease lease;
    const auto target = backend_.probe();
    if (!target_eligible(target, now) ||
        (expected_target != 0 && target.hardware_id != expected_target)) reject();
    // The restricted helper repeats destination checks in its write session.
    backend_.write(target.hardware_id, record_, contacts_ ? &*contacts_ : nullptr);
    auto observed = backend_.snapshot(target.hardware_id);
    if (!std::ranges::equal(record_.bytes(), observed.bytes())) reject();
    if (contacts_) {
      const auto copied = backend_.contact_region(target.hardware_id);
      if (!copied || !std::ranges::equal(contacts_->bytes(), copied->bytes())) reject();
    }
    cancel();
  } catch (...) { cancel(); throw; }
}
}  // namespace usb_vault::card::vikey
