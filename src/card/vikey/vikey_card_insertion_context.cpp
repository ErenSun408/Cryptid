#include "card/vikey/vikey_card_insertion_context.h"

#include "usb_vault/card/card_error.h"

#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void reject_invalid_probe() {
  throw std::system_error(
      make_error_code(CardError::invalid_configuration),
      "ViKey insertion context is inconsistent");
}

void validate_product_state(const CardRecordState state) {
  switch (state) {
    case CardRecordState::provisioned_uninitialized:
    case CardRecordState::initialized_v1:
    case CardRecordState::unsupported_version:
    case CardRecordState::malformed:
      return;
    case CardRecordState::legacy_p1_blob:
      reject_invalid_probe();
  }
  reject_invalid_probe();
}

void validate_probe(const VikeyCardMediaProbe& probe) {
  if (probe.hardware_id == 0) reject_invalid_probe();

  switch (probe.media_state) {
    case VikeyCardMediaState::blank:
    case VikeyCardMediaState::legacy_p1_or_foreign_prefix:
    case VikeyCardMediaState::foreign_data:
    case VikeyCardMediaState::provisioning_interrupted:
    case VikeyCardMediaState::conflicting_evidence:
      if (probe.logical_state.has_value()) reject_invalid_probe();
      return;

    case VikeyCardMediaState::product_evidence:
      if (!probe.logical_state.has_value()) reject_invalid_probe();
      validate_product_state(*probe.logical_state);
      return;
  }
  reject_invalid_probe();
}

}  // namespace

VikeyCardInsertionContext::VikeyCardInsertionContext(
    VikeyCardMediaProbe probe)
    : probe_(std::move(probe)) {
  validate_probe(probe_);
}

VikeyCardInsertionContext::VikeyCardInsertionContext(
    VikeyCardMediaProbe probe, const CardProductRecordState journal_state)
    : probe_(std::move(probe)), journal_state_(journal_state) {
  validate_probe(probe_);
  if (probe_.media_state != VikeyCardMediaState::product_evidence ||
      probe_.logical_state != CardRecordState::unsupported_version) {
    reject_invalid_probe();
  }
  switch (journal_state) {
    case CardProductRecordState::initialized_v2:
    case CardProductRecordState::unsupported_version:
    case CardProductRecordState::malformed:
      return;
    default:
      reject_invalid_probe();
  }
}

application::CardEntryMediaEvidence
VikeyCardInsertionContext::inspect_media() {
  switch (probe_.media_state) {
    case VikeyCardMediaState::blank:
    case VikeyCardMediaState::provisioning_interrupted:
      return application::CardEntryMediaEvidence::initializable_media;

    case VikeyCardMediaState::product_evidence:
      return application::CardEntryMediaEvidence::product_record;

    case VikeyCardMediaState::legacy_p1_or_foreign_prefix:
    case VikeyCardMediaState::foreign_data:
    case VikeyCardMediaState::conflicting_evidence:
      return application::CardEntryMediaEvidence::blocked;
  }
  return application::CardEntryMediaEvidence::blocked;
}

CardRecordState
VikeyCardInsertionContext::inspect_product_record_read_only() {
  if (probe_.media_state != VikeyCardMediaState::product_evidence ||
      !probe_.logical_state.has_value()) {
    throw std::system_error(
        make_error_code(CardError::invalid_operation),
        "ViKey insertion context does not contain product evidence");
  }
  return *probe_.logical_state;
}

CardProductRecordState
VikeyCardInsertionContext::inspect_supported_product_record_read_only() {
  if (journal_state_.has_value()) return *journal_state_;
  return product_state_from_legacy(inspect_product_record_read_only());
}

const VikeyCardMediaProbe& VikeyCardInsertionContext::probe() const noexcept {
  return probe_;
}

}  // namespace usb_vault::card::vikey
