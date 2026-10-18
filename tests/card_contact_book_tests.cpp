#include "usb_vault/application/card_contact_book.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardContactBook;
using usb_vault::application::ContactBookChange;
using usb_vault::application::ICardOperationLease;
using usb_vault::contacts::ContactIdentity;
using usb_vault::contacts::ContactPublicKey;
using usb_vault::contacts::ContactStoreState;
using usb_vault::contacts::IContactStoreStorage;
using usb_vault::contacts::encode_contact_key_file;
using usb_vault::contacts::decode_contact_key_file;
using usb_vault::contacts::kContactStoreV1MaxContacts;
using usb_vault::contacts::kContactStoreV1RegionEnd;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

struct Card final {
  std::vector<std::byte> bytes = std::vector<std::byte>(kContactStoreV1RegionEnd);
  int leases_alive{0};
  int leases_taken{0};
  int writes{0};
};

class Storage final : public IContactStoreStorage {
 public:
  explicit Storage(Card& card) : card_(card) {}
  std::size_t capacity() override { return card_.bytes.size(); }
  SecureBuffer read(std::size_t offset, std::size_t length) override {
    require(card_.leases_alive == 1, "the card was read without a lease");
    SecureBuffer out(length);
    std::copy_n(card_.bytes.begin() + static_cast<std::ptrdiff_t>(offset), length,
                out.data());
    return out;
  }
  void write(std::size_t offset, std::span<const std::byte> data) override {
    require(card_.leases_alive == 1, "the card was written without a lease");
    ++card_.writes;
    std::ranges::copy(data, card_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  }

 private:
  Card& card_;
};

class Lease final : public ICardOperationLease {
 public:
  explicit Lease(Card& card) : card_(card) {
    ++card_.leases_alive;
    ++card_.leases_taken;
  }
  ~Lease() noexcept override { --card_.leases_alive; }

 private:
  Card& card_;
};

[[nodiscard]] SecureBuffer root(const unsigned char fill) {
  SecureBuffer secret(32);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    secret.data()[i] = static_cast<std::byte>(fill + i);
  }
  return secret;
}

[[nodiscard]] std::unique_ptr<CardContactBook> open_book(
    CardAuthenticationSession& session, Card& card) {
  auto book = std::make_unique<CardContactBook>(
      session, std::make_unique<Storage>(card),
      [&card] { return std::make_unique<Lease>(card); });
  static_cast<void>(book->load());
  return book;
}

[[nodiscard]] std::string key_file(const unsigned char fill) {
  return encode_contact_key_file(ContactIdentity::derive(root(fill)).public_key());
}

void test_requires_authentication() {
  CardAuthenticationSession session;
  Card card;
  bool refused = false;
  try {
    CardContactBook book(session, std::make_unique<Storage>(card),
                         [&card] { return std::make_unique<Lease>(card); });
  } catch (const std::logic_error&) {
    refused = true;
  }
  require(refused, "an unauthenticated session opened a contact book");

  session.establish(root(1));
  bool no_storage = false;
  try {
    CardContactBook book(session, nullptr, [&card] { return std::make_unique<Lease>(card); });
  } catch (const std::invalid_argument&) {
    no_storage = true;
  }
  bool no_lease = false;
  try {
    CardContactBook book(session, std::make_unique<Storage>(card), {});
  } catch (const std::invalid_argument&) {
    no_lease = true;
  }
  require(no_storage && no_lease, "a book without storage or lease was opened");

  CardContactBook nulls(session, std::make_unique<Storage>(card),
                        [] { return std::unique_ptr<ICardOperationLease>{}; });
  bool unleased = false;
  try {
    static_cast<void>(nulls.load());
  } catch (const std::runtime_error&) {
    unleased = true;
  }
  require(unleased && card.writes == 0, "a missing lease did not stop card access");
}

void test_ended_login_refuses_every_call() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  auto book = open_book(session, card);
  require(book->add(key_file(10), "A").change == ContactBookChange::done, "seed");
  const ContactPublicKey key = book->entries()[0].public_key;
  session.clear_authentication();
  const auto refused = [](auto&& call) {
    try {
      call();
    } catch (const std::logic_error&) {
      return true;
    }
    return false;
  };
  require(refused([&] { static_cast<void>(book->identity()); }) &&
              refused([&] { static_cast<void>(book->load()); }) &&
              refused([&] { static_cast<void>(book->add(key_file(11), "B")); }) &&
              refused([&] { static_cast<void>(book->rename(key, "C")); }) &&
              refused([&] { static_cast<void>(book->remove(key)); }) &&
              refused([&] { static_cast<void>(book->remark_for(key)); }) &&
              refused([&] { static_cast<void>(book->own_key_file()); }),
          "a book outlived its logout");
  // A new login of the same card does not revive the old book either.
  session.establish(root(1));
  require(refused([&] { static_cast<void>(book->identity()); }),
          "a book survived into a later login");
}

void test_stale_book_and_unusable_states() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  auto first = open_book(session, card);
  auto second = open_book(session, card);
  require(first->add(key_file(10), "A").change == ContactBookChange::done, "seed");
  const int leases = card.leases_taken;
  bool changed = false;
  try {
    static_cast<void>(second->add(key_file(11), "B"));
  } catch (const usb_vault::contacts::ContactStoreChangedError&) {
    changed = true;
  }
  require(changed && second->entries().size() == 1 &&
              card.leases_taken == leases + 1,
          "a stale book wrote or did not reload under one lease");

  CardContactBook unread(session, std::make_unique<Storage>(card),
                         [&card] { return std::make_unique<Lease>(card); });
  const int writes = card.writes;
  bool refused = false;
  try {
    static_cast<void>(unread.add(key_file(12), "C"));
  } catch (const std::logic_error&) {
    refused = true;
  }
  require(refused && card.writes == writes, "an unread book accepted an add");
}

void test_identity_follows_the_root() {
  Card card;
  CardAuthenticationSession first;
  first.establish(root(1));
  CardAuthenticationSession again;
  again.establish(root(1));
  CardAuthenticationSession other;
  other.establish(root(2));
  const auto a = open_book(first, card);
  const auto b = open_book(again, card);
  const auto c = open_book(other, card);
  require(a->identity().public_key() == b->identity().public_key(),
          "the same root gave two identities");
  require(a->identity().public_key() != c->identity().public_key(),
          "two roots gave one identity");
  const auto decoded = decode_contact_key_file(a->own_key_file());
  require(decoded.public_key && *decoded.public_key == a->identity().public_key(),
          "the own key file does not hold the own key");
  require(a->own_fingerprint().size() == 19, "the own fingerprint is malformed");
  require(a->state() == ContactStoreState::empty && card.writes == 0,
          "opening a book wrote the card");
}

void test_add_rename_remove() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  auto book = open_book(session, card);

  require(book->add(key_file(10), "  张三 ").change == ContactBookChange::done,
          "a valid contact was not added");
  require(book->entries().size() == 1 && book->entries()[0].remark == "张三",
          "the added contact was not normalized and listed");
  const ContactPublicKey zhang = book->entries()[0].public_key;
  require(book->remark_for(zhang) == std::string("张三"), "remark_for missed");
  require(card.leases_alive == 0 && card.leases_taken >= 2,
          "a save did not hold and release a lease");

  const int writes = card.writes;
  require(book->rename(zhang, "张三") .change == ContactBookChange::done &&
              card.writes == writes,
          "an unchanged remark was written");
  require(book->rename(zhang, "老张").change == ContactBookChange::done &&
              book->remark_for(zhang) == std::string("老张"),
          "a remark was not changed");

  auto reopened = open_book(session, card);
  require(reopened->entries() == book->entries(), "the book did not persist");

  require(book->remove(zhang).change == ContactBookChange::done &&
              book->entries().empty(),
          "a contact was not removed");
  require(book->remove(zhang).change == ContactBookChange::not_found &&
              book->rename(zhang, "x").change == ContactBookChange::not_found,
          "a missing contact was not reported");
}

void test_refusals_before_writing() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  auto book = open_book(session, card);
  require(book->add(key_file(10), "A").change == ContactBookChange::done, "seed add");
  const int writes = card.writes;

  require(book->add(book->own_key_file(), "me").change == ContactBookChange::own_key,
          "the own key was added");
  ContactPublicKey twin = book->identity().public_key();
  twin[31] ^= std::byte{0x80};
  require(usb_vault::contacts::contact_public_key_usable(twin),
          "the sign-bit twin fixture is not usable");
  require(book->add(encode_contact_key_file(twin), "me").change ==
              ContactBookChange::own_key,
          "the own key's sign-bit twin was added");
  ContactPublicKey contact_twin = book->entries()[0].public_key;
  contact_twin[31] ^= std::byte{0x80};
  const auto twin_added = book->add(encode_contact_key_file(contact_twin), "B");
  require(twin_added.change == ContactBookChange::already_present &&
              twin_added.existing_remark == std::string("A") &&
              book->remark_for(contact_twin) == std::string("A"),
          "a contact's sign-bit twin was not the same recipient");
  const auto duplicate = book->add(key_file(10), "B");
  require(duplicate.change == ContactBookChange::already_present &&
              duplicate.existing_remark == std::string("A"),
          "a duplicate did not name the existing remark");
  require(book->add("not a key file", "B").change == ContactBookChange::malformed_file,
          "a foreign file was not malformed");
  std::string future = key_file(11);
  future.replace(0, 24, "USB-VAULT-CONTACT-KEY v2");
  require(book->add(future, "B").change == ContactBookChange::unsupported_version,
          "a future key file was not unsupported");
  std::string corrupted = key_file(11);
  corrupted[30] = corrupted[30] == '2' ? '3' : '2';
  const auto corrupt = book->add(corrupted, "B").change;
  require(corrupt == ContactBookChange::checksum_mismatch ||
              corrupt == ContactBookChange::malformed_file,
          "a corrupted key file was accepted");
  ContactPublicKey neutral{};
  neutral[0] = std::byte{1};
  require(book->add(encode_contact_key_file(neutral), "B").change ==
              ContactBookChange::unusable_key,
          "an unusable key was accepted");
  require(book->add(key_file(11), " ").change == ContactBookChange::empty_remark,
          "an empty remark was accepted");
  require(book->add(key_file(11), std::string(32, 'x')).change ==
              ContactBookChange::remark_too_long,
          "a long remark was accepted");
  require(book->add(key_file(11), "a\x01").change == ContactBookChange::invalid_remark,
          "a control character was accepted");
  require(card.writes == writes, "a refused change wrote the card");
}

void test_list_full() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  auto book = open_book(session, card);
  for (std::size_t i = 0; i < kContactStoreV1MaxContacts; ++i) {
    require(book->add(key_file(static_cast<unsigned char>(100 + i)),
                      "c" + std::to_string(i))
                    .change == ContactBookChange::done,
            "a contact below the limit was refused");
  }
  require(book->add(key_file(200), "extra").change == ContactBookChange::list_full,
          "a 49th contact was accepted");
}

void test_damaged_book_recovers() {
  Card card;
  CardAuthenticationSession session;
  session.establish(root(1));
  {
    auto book = open_book(session, card);
    require(book->add(key_file(10), "A").change == ContactBookChange::done, "seed");
  }
  // Damage the body of the one written copy (descriptor still authentic).
  card.bytes[1808 + 64 + 40] ^= std::byte{1};
  auto book = open_book(session, card);
  require(book->state() == ContactStoreState::damaged, "damage was not reported");
  bool refused = false;
  try {
    static_cast<void>(book->add(key_file(11), "B"));
  } catch (const std::logic_error&) {
    refused = true;
  }
  require(refused, "a damaged book accepted an add");
  book->discard_and_clear();
  require(book->state() == ContactStoreState::loaded && book->entries().empty(),
          "discarding did not leave an empty book");
}

}  // namespace

int main() {
  try {
    test_requires_authentication();
    test_identity_follows_the_root();
    test_ended_login_refuses_every_call();
    test_stale_book_and_unusable_states();
    test_add_rename_remove();
    test_refusals_before_writing();
    test_list_full();
    test_damaged_book_recovers();
  } catch (const std::exception& error) {
    std::cerr << "card_contact_book_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "card_contact_book_tests passed\n";
  return 0;
}
