#include "card/vikey/vikey_card_backup.h"
#include "card/vikey/card_recheck_coordinator.h"
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
void VikeyHelperCardBackupBackend::write(std::uint32_t identity,
                                        const security::SecureBuffer& record) {
  auto c = credentials_.load_credentials();
  write_vikey_development_card_record_with_network_access(
      config_.helper_path, config_.helper_digest, config_.vendor_directory,
      config_.vendor_digest, identity, c.user_sn, c.api_key, c.user_pin,
      record, config_.write_timeout, false, true);
}
void VikeyCardBackup::cancel() noexcept {
  record_.clear(); source_ = 0; deadline_ = {};
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
    auto second = backend_.snapshot(source.hardware_id);
    const auto confirmed = backend_.probe();
    if (!initialized(confirmed) || confirmed.hardware_id != source.hardware_id ||
        inspect_card_record(first.bytes()) != CardRecordState::initialized_v1 ||
        !std::ranges::equal(first.bytes(), second.bytes())) reject();
    record_ = std::move(first);
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
    backend_.write(target.hardware_id, record_);
    auto observed = backend_.snapshot(target.hardware_id);
    if (!std::ranges::equal(record_.bytes(), observed.bytes())) reject();
    cancel();
  } catch (...) { cancel(); throw; }
}
}  // namespace usb_vault::card::vikey
