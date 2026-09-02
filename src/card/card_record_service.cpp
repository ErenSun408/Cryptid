#include "usb_vault/card/card_record_service.h"

#include "usb_vault/card/card_error.h"

#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

namespace usb_vault::card {
namespace {

[[nodiscard]] bool equal_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size()) return false;
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0;
}

// The record primitive needs the caller's password twice: once to open the
// existing record and once as one of the two replacement envelopes.
// SecureBuffer is deliberately not copyable, so the duplicate is explicit and
// is consumed by the same call that consumes the original.
[[nodiscard]] security::SecureBuffer duplicate(
    const security::SecureBuffer& source) {
  security::SecureBuffer result(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    result.bytes()[index] = source.bytes()[index];
  }
  return result;
}

[[noreturn]] void fail_unverified_write() {
  throw std::system_error(
      make_error_code(CardError::device_failure),
      "card record update outcome could not be verified");
}

}  // namespace

UnexpectedCardRecordState::UnexpectedCardRecordState(
    const CardRecordState observed)
    : std::runtime_error("card record is not in the required product state"),
      observed_(observed) {}

CardRecordState UnexpectedCardRecordState::observed() const noexcept {
  return observed_;
}

CardRecordService::CardRecordService(ICardStore& store) noexcept
    : store_(store) {}

CardRecordState CardRecordService::inspect() {
  security::SecureBuffer record = store_.get_blob();
  return inspect_card_record(record.bytes());
}

security::SecureBuffer CardRecordService::initialize_and_authenticate(
    security::SecureBuffer daily_password_utf8) {
  security::SecureBuffer current = store_.get_blob();
  const CardRecordState state = inspect_card_record(current.bytes());
  if (state != CardRecordState::provisioned_uninitialized) {
    daily_password_utf8.clear();
    throw UnexpectedCardRecordState(state);
  }

  InitializedCardRecordV1 initialized =
      initialize_card_record_and_root_v1(
          std::move(daily_password_utf8));
  security::SecureBuffer before_write = store_.get_blob();
  const CardRecordState before_write_state =
      inspect_card_record(before_write.bytes());
  if (before_write_state !=
      CardRecordState::provisioned_uninitialized) {
    initialized.root_secret.clear();
    throw UnexpectedCardRecordState(before_write_state);
  }

  std::exception_ptr write_failure;
  try {
    store_.put_blob(initialized.record);
  } catch (...) {
    write_failure = std::current_exception();
  }

  security::SecureBuffer observed;
  try {
    observed = store_.get_blob();
  } catch (...) {
    initialized.root_secret.clear();
    fail_unverified_write();
  }
  if (!equal_bytes(observed.bytes(), initialized.record.bytes())) {
    initialized.root_secret.clear();
    if (write_failure) std::rethrow_exception(write_failure);
    fail_unverified_write();
  }
  return std::move(initialized.root_secret);
}

void CardRecordService::set_second_password(
    security::SecureBuffer existing_password_utf8,
    security::SecureBuffer second_password_utf8) {
  if (existing_password_utf8.empty() || second_password_utf8.empty()) {
    existing_password_utf8.clear();
    second_password_utf8.clear();
    throw std::invalid_argument(
        "both card record passwords are required");
  }

  security::SecureBuffer record = store_.get_blob();
  const CardRecordState state = inspect_card_record(record.bytes());
  if (state != CardRecordState::initialized_v1) {
    existing_password_utf8.clear();
    second_password_utf8.clear();
    throw UnexpectedCardRecordState(state);
  }

  // The newly chosen password becomes the primary envelope and the existing
  // one the secondary. Unlocking tries both and returns the same root, so the
  // order carries no meaning beyond keeping the layout fixed.
  security::SecureBuffer retained = duplicate(existing_password_utf8);
  security::SecureBuffer updated = rewrap_card_record_v1(
      record.bytes(), std::move(existing_password_utf8),
      std::move(second_password_utf8), std::move(retained));

  std::exception_ptr write_failure;
  try {
    store_.put_blob(updated);
  } catch (...) {
    write_failure = std::current_exception();
  }

  security::SecureBuffer observed;
  try {
    observed = store_.get_blob();
  } catch (...) {
    fail_unverified_write();
  }
  if (!equal_bytes(observed.bytes(), updated.bytes())) {
    if (write_failure) std::rethrow_exception(write_failure);
    fail_unverified_write();
  }
}

void CardRecordService::reset_to_provisioning() {
  security::SecureBuffer current = store_.get_blob();
  const CardRecordState state = inspect_card_record(current.bytes());
  if (state != CardRecordState::initialized_v1) {
    throw UnexpectedCardRecordState(state);
  }

  // The marker is the same public framing the controlled provisioning process
  // writes, so a reset card is byte-for-byte an unused one. No record of the
  // identity it replaced survives anywhere on the card.
  const security::SecureBuffer marker =
      provision_uninitialized_card_record_v1();

  std::exception_ptr write_failure;
  try {
    store_.put_blob(marker);
  } catch (...) {
    write_failure = std::current_exception();
  }

  security::SecureBuffer observed;
  try {
    observed = store_.get_blob();
  } catch (...) {
    fail_unverified_write();
  }
  if (!equal_bytes(observed.bytes(), marker.bytes())) {
    if (write_failure) std::rethrow_exception(write_failure);
    fail_unverified_write();
  }
}

security::SecureBuffer CardRecordService::authenticate(
    security::SecureBuffer password_utf8) {
  security::SecureBuffer record = store_.get_blob();
  const CardRecordState state = inspect_card_record(record.bytes());
  if (state != CardRecordState::initialized_v1) {
    password_utf8.clear();
    throw UnexpectedCardRecordState(state);
  }
  return unlock_card_record_v1(record.bytes(), std::move(password_utf8));
}

}  // namespace usb_vault::card
