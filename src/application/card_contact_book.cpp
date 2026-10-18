#include "usb_vault/application/card_contact_book.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace usb_vault::application {
namespace {

using contacts::ContactEntry;
using contacts::ContactPublicKey;

[[nodiscard]] const security::SecureBuffer& require_root(
    const security::SecureBuffer& root, const bool authenticated) {
  if (!authenticated) {
    throw std::logic_error("a contact book needs an authenticated session");
  }
  return root;
}

[[nodiscard]] ContactBookChange remark_change(
    const contacts::ContactRemarkError error) noexcept {
  switch (error) {
    case contacts::ContactRemarkError::empty:
      return ContactBookChange::empty_remark;
    case contacts::ContactRemarkError::too_long:
      return ContactBookChange::remark_too_long;
    case contacts::ContactRemarkError::none:
    case contacts::ContactRemarkError::invalid_text:
      break;
  }
  return ContactBookChange::invalid_remark;
}

[[nodiscard]] ContactBookChange key_file_change(
    const contacts::ContactKeyFileError error) noexcept {
  switch (error) {
    case contacts::ContactKeyFileError::unsupported_version:
      return ContactBookChange::unsupported_version;
    case contacts::ContactKeyFileError::checksum_mismatch:
      return ContactBookChange::checksum_mismatch;
    case contacts::ContactKeyFileError::unusable_key:
      return ContactBookChange::unusable_key;
    case contacts::ContactKeyFileError::none:
    case contacts::ContactKeyFileError::malformed:
      break;
  }
  return ContactBookChange::malformed_file;
}

// Keys differing only in the Ed25519 sign bit are the same recipient.
[[nodiscard]] bool same_recipient(const ContactPublicKey& a,
                                  const ContactPublicKey& b) {
  return contacts::contact_x25519_public_key(a) ==
         contacts::contact_x25519_public_key(b);
}

}  // namespace

CardContactBook::CardContactBook(
    const CardAuthenticationSession& authentication,
    std::unique_ptr<contacts::IContactStoreStorage> storage, LeaseFactory lease)
    : authentication_(authentication),
      generation_(authentication.generation()),
      identity_(contacts::ContactIdentity::derive(
          require_root(authentication.root_secret_, authentication.authenticated()))),
      storage_(storage ? std::move(storage)
                       : throw std::invalid_argument("a contact book needs storage")),
      store_(*storage_,
             require_root(authentication.root_secret_, authentication.authenticated())),
      lease_(std::move(lease)) {
  if (!lease_) throw std::invalid_argument("a contact book needs a card lease");
}

void CardContactBook::require_current() const {
  if (!authentication_.authenticated() ||
      authentication_.generation() != generation_) {
    throw std::logic_error("the contact book belongs to an ended login");
  }
}

std::unique_ptr<ICardOperationLease> CardContactBook::take_lease() const {
  auto lease = lease_();
  if (!lease) throw std::runtime_error("the card operation lease is unavailable");
  return lease;
}

const contacts::ContactIdentity& CardContactBook::identity() const {
  require_current();
  return identity_;
}

contacts::ContactIdentity CardContactBook::transfer_identity() const {
  require_current();
  return identity_.clone();
}

contacts::ContactStoreState CardContactBook::load() {
  require_current();
  const auto lease = take_lease();
  return store_.load();
}

std::string CardContactBook::own_key_file() const {
  require_current();
  return contacts::encode_contact_key_file(identity_.public_key());
}

std::string CardContactBook::own_fingerprint() const {
  require_current();
  return contacts::format_contact_fingerprint(identity_.public_key());
}

std::optional<std::string> CardContactBook::remark_for(
    const ContactPublicKey& key) const {
  require_current();
  for (const ContactEntry& entry : store_.entries()) {
    if (same_recipient(entry.public_key, key)) return entry.remark;
  }
  return std::nullopt;
}

ContactBookOutcome CardContactBook::add(const std::string_view key_file_text,
                                        const std::string_view remark) {
  require_current();
  const contacts::ContactKeyFileDecoding decoded =
      contacts::decode_contact_key_file(key_file_text);
  if (!decoded.public_key) return {.change = key_file_change(decoded.error)};
  const ContactPublicKey& key = *decoded.public_key;
  if (same_recipient(key, identity_.public_key())) {
    return {.change = ContactBookChange::own_key};
  }
  for (const ContactEntry& entry : store_.entries()) {
    if (same_recipient(entry.public_key, key)) {
      return {.change = ContactBookChange::already_present,
              .existing_remark = entry.remark};
    }
  }
  const contacts::ContactRemarkNormalization normalized =
      contacts::normalize_contact_remark(remark);
  if (!normalized.remark) return {.change = remark_change(normalized.error)};
  if (store_.entries().size() >= contacts::kContactStoreV1MaxContacts) {
    return {.change = ContactBookChange::list_full};
  }
  std::vector<ContactEntry> next = store_.entries();
  next.push_back({.public_key = key, .remark = *normalized.remark});
  save(next);
  return {};
}

ContactBookOutcome CardContactBook::rename(const ContactPublicKey& key,
                                           const std::string_view remark) {
  require_current();
  std::vector<ContactEntry> next = store_.entries();
  const auto found = std::ranges::find(next, key, &ContactEntry::public_key);
  if (found == next.end()) return {.change = ContactBookChange::not_found};
  const contacts::ContactRemarkNormalization normalized =
      contacts::normalize_contact_remark(remark);
  if (!normalized.remark) return {.change = remark_change(normalized.error)};
  if (found->remark == *normalized.remark) return {};
  found->remark = *normalized.remark;
  save(next);
  return {};
}

ContactBookOutcome CardContactBook::remove(const ContactPublicKey& key) {
  require_current();
  std::vector<ContactEntry> next = store_.entries();
  const auto found = std::ranges::find(next, key, &ContactEntry::public_key);
  if (found == next.end()) return {.change = ContactBookChange::not_found};
  next.erase(found);
  save(next);
  return {};
}

void CardContactBook::discard_and_clear() {
  require_current();
  const auto lease = take_lease();
  store_.discard_and_clear();
}

void CardContactBook::save(const std::vector<ContactEntry>& entries) {
  const auto lease = take_lease();
  store_.save(entries);
}

}  // namespace usb_vault::application
