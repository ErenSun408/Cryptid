#pragma once

#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_operation_lease.h"
#include "usb_vault/contacts/contact_identity_v1.h"
#include "usb_vault/contacts/contact_store_v1.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace usb_vault::application {

enum class ContactBookChange : std::uint8_t {
  done,
  malformed_file,
  unsupported_version,
  checksum_mismatch,
  unusable_key,
  own_key,
  already_present,
  list_full,
  empty_remark,
  remark_too_long,
  invalid_remark,
  not_found,
};

struct ContactBookOutcome final {
  ContactBookChange change{ContactBookChange::done};
  // For already_present: the remark the key is already listed under.
  std::optional<std::string> existing_remark;
};

// One login's contact list and identity (P5.14). Both come from the card root
// secret, so every password of the card, and every backup card, sees the same
// book. Writes go to the card at once; each one holds one card operation lease
// across the store's read, write and read-back, so no other card operation in
// this or another instance interleaves. Not thread-safe: the session worker
// serializes calls. Bound to the login it was opened in: once that session
// is cleared or re-established every call throws std::logic_error, so the
// keys it derived cannot sign, decrypt or write the card for another login.
// Destroy it with the login; the identity's private keys are wiped with it.
class CardContactBook final {
 public:
  using LeaseFactory = std::function<std::unique_ptr<ICardOperationLease>()>;

  // `authentication` must be authenticated; throws std::logic_error otherwise.
  // Nothing is read from the card until load().
  CardContactBook(const CardAuthenticationSession& authentication,
                  std::unique_ptr<contacts::IContactStoreStorage> storage,
                  LeaseFactory lease);

  CardContactBook(const CardContactBook&) = delete;
  CardContactBook& operator=(const CardContactBook&) = delete;

  contacts::ContactStoreState load();
  [[nodiscard]] contacts::ContactStoreState state() const noexcept {
    return store_.state();
  }
  [[nodiscard]] const std::vector<contacts::ContactEntry>& entries() const noexcept {
    return store_.entries();
  }

  // Throws std::logic_error once the login has ended. The reference is valid
  // only while the book is; work that may outlive it uses transfer_identity().
  [[nodiscard]] const contacts::ContactIdentity& identity() const;
  // A copy of the identity that the caller owns, for an export running on a
  // worker after the book may already be gone. Checked against the login now.
  [[nodiscard]] contacts::ContactIdentity transfer_identity() const;
  [[nodiscard]] std::string own_key_file() const;
  [[nodiscard]] std::string own_fingerprint() const;

  // The remark listed for this recipient, matched by X25519 key so a listed
  // sign-bit twin still names the sender whose signature verified.
  [[nodiscard]] std::optional<std::string> remark_for(
      const contacts::ContactPublicKey& key) const;

  // Store failures (ContactStoreChangedError, card errors) propagate after the
  // store has reloaded; a refusal decided before writing is returned instead.
  ContactBookOutcome add(std::string_view key_file_text, std::string_view remark);
  ContactBookOutcome rename(const contacts::ContactPublicKey& key,
                            std::string_view remark);
  ContactBookOutcome remove(const contacts::ContactPublicKey& key);
  // Recovery for the damaged and inconsistent states.
  void discard_and_clear();

 private:
  void require_current() const;
  [[nodiscard]] std::unique_ptr<ICardOperationLease> take_lease() const;
  void save(const std::vector<contacts::ContactEntry>& entries);

  // Declared first: checked before anything is derived from the root.
  const CardAuthenticationSession& authentication_;
  std::uint64_t generation_{};
  contacts::ContactIdentity identity_;
  std::unique_ptr<contacts::IContactStoreStorage> storage_;
  contacts::ContactStoreV1 store_;
  LeaseFactory lease_;
};

}  // namespace usb_vault::application
