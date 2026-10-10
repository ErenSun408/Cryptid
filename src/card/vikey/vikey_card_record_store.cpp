#include "card/vikey/vikey_card_record_store.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <system_error>

namespace usb_vault::card::vikey {
namespace {

// The logical record has two envelopes, or three once a third password is set.
[[nodiscard]] bool valid_record_size(const std::size_t size) noexcept {
  return size == kCardRecordV1Size || size == kCardRecordV1ThreeSlotSize;
}

}  // namespace

VikeyCardRecordStore::VikeyCardRecordStore(
    IVikeyCardRecordBackend& backend) noexcept
    : backend_(backend) {}

security::SecureBuffer VikeyCardRecordStore::get_blob() {
  security::SecureBuffer record = backend_.read_record();
  if (!valid_record_size(record.size())) {
    throw std::system_error(
        make_error_code(CardError::device_failure),
        "ViKey backend returned an invalid card record size");
  }
  return record;
}

void VikeyCardRecordStore::put_blob(
    const security::SecureBuffer& blob) {
  if (!valid_record_size(blob.size())) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey card record must contain 216 or 292 bytes");
  }
  backend_.write_record(blob);
}

CardCapabilities VikeyCardRecordStore::capabilities() const noexcept {
  return {};
}

}  // namespace usb_vault::card::vikey
