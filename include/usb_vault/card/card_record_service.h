#pragma once

#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/card/card_store.h"

#include <stdexcept>

namespace usb_vault::card {

class UnexpectedCardRecordState final : public std::runtime_error {
 public:
  explicit UnexpectedCardRecordState(CardRecordState observed);

  [[nodiscard]] CardRecordState observed() const noexcept;

 private:
  CardRecordState observed_;
};

// Product-level lifecycle boundary over the unchanged single-blob card
// contract. It classifies public framing before password work, allows product
// initialization only from the exact provisioning marker, and returns only a
// volatile root secret after initialization or authentication succeeds.
class CardRecordService final {
 public:
  explicit CardRecordService(ICardStore& store) noexcept;

  [[nodiscard]] CardRecordState inspect();

  // Consumes the password, rechecks the exact provisioning state after the
  // KDF, commits an initialized record, then re-reads the card. A reported
  // write failure is accepted only when a fresh read proves the exact intended
  // record became active. Returns S only after that proof so the caller can
  // enter an authenticated no-volume session.
  [[nodiscard]] security::SecureBuffer initialize_and_authenticate(
      security::SecureBuffer daily_password_utf8);

  // Consumes the password. Only an initialized v1 record reaches the KDF;
  // provisioning, legacy, unsupported, and malformed states are reported via
  // UnexpectedCardRecordState and never converted into initialization.
  [[nodiscard]] security::SecureBuffer authenticate(
      security::SecureBuffer password_utf8);

 private:
  ICardStore& store_;
};

}  // namespace usb_vault::card
