#include "usb_vault/contacts/contact_store_v1.h"

#include "contacts/contact_store_v1_format.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using usb_vault::contacts::ContactEntriesError;
using usb_vault::contacts::ContactEntry;
using usb_vault::contacts::ContactIdentity;
using usb_vault::contacts::ContactRemarkError;
using usb_vault::contacts::ContactStoreState;
using usb_vault::contacts::ContactStoreChangedError;
using usb_vault::contacts::ContactStoreV1;
using usb_vault::contacts::IContactStoreStorage;
using usb_vault::contacts::kContactStoreV1CopyOffsets;
using usb_vault::contacts::kContactStoreV1CopySize;
using usb_vault::contacts::kContactStoreV1MaxContacts;
using usb_vault::contacts::kContactStoreV1RegionEnd;
using usb_vault::contacts::kContactStoreV1RegionOffset;
using usb_vault::contacts::normalize_contact_remark;
using usb_vault::contacts::validate_contact_entries;
using usb_vault::security::SecureBuffer;
namespace detail = usb_vault::contacts::detail;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class MemoryStorage final : public IContactStoreStorage {
 public:
  explicit MemoryStorage(const std::size_t capacity, const unsigned char fill)
      : bytes_(capacity, static_cast<std::byte>(fill)) {}

  std::size_t capacity() override { return bytes_.size(); }

  SecureBuffer read(const std::size_t offset,
                    const std::size_t length) override {
    if (offset + length > bytes_.size()) throw std::out_of_range("read");
    ++reads;
    if (fail_reads) throw std::runtime_error("card not readable");
    if (garble_next_read) {
      garble_next_read = false;
      SecureBuffer noise(length);
      for (std::size_t i = 0; i < length; ++i) {
        noise.data()[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
      }
      return noise;
    }
    SecureBuffer out(length);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), length,
                out.data());
    return out;
  }

  void write(const std::size_t offset,
             std::span<const std::byte> data) override {
    if (offset + data.size() > bytes_.size()) throw std::out_of_range("write");
    writes.push_back({offset, data.size()});
    if (drop_writes) return;
    const std::size_t kept =
        tear_after ? std::min(*tear_after, data.size()) : data.size();
    std::copy_n(data.begin(), kept,
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    if (tear_after) {
      if (fail_reads_after_tear) fail_reads = true;
      throw std::runtime_error("card removed during write");
    }
  }

  std::vector<std::byte> bytes_;
  std::optional<std::size_t> tear_after;
  bool drop_writes{false};
  bool fail_reads{false};
  bool fail_reads_after_tear{false};
  bool garble_next_read{false};
  int reads{0};
  std::vector<std::pair<std::size_t, std::size_t>> writes;
};

[[nodiscard]] SecureBuffer secret_of(const unsigned char fill) {
  SecureBuffer secret(32);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    secret.data()[i] = static_cast<std::byte>(fill + i);
  }
  return secret;
}

[[nodiscard]] ContactEntry contact(const unsigned char seed,
                                   const std::string& remark) {
  return ContactEntry{ContactIdentity::derive(secret_of(seed)).public_key(),
                      remark};
}

[[nodiscard]] std::vector<ContactEntry> sample(const std::size_t count) {
  std::vector<ContactEntry> entries;
  for (std::size_t i = 0; i < count; ++i) {
    entries.push_back(contact(static_cast<unsigned char>(100 + i),
                              "c" + std::to_string(i)));
  }
  return entries;
}

template <typename F>
[[nodiscard]] bool throws(F&& f) {
  try {
    f();
  } catch (...) {
    return true;
  }
  return false;
}

void test_unavailable_on_small_card() {
  MemoryStorage storage(kContactStoreV1RegionEnd - 1, 0);
  ContactStoreV1 store(storage, secret_of(1));
  require(store.state() == ContactStoreState::unread, "initial state is not unread");
  require(throws([&] { store.save(sample(1)); }),
          "an unread store accepted a save");
  require(store.load() == ContactStoreState::unavailable,
          "a small card was not unavailable");
  require(storage.reads == 0, "a small card was read");
  require(throws([&] { store.save(sample(1)); }),
          "an unavailable store accepted a save");
}

void test_blank_and_foreign_regions_are_empty() {
  for (const unsigned char fill : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x5A}}) {
    MemoryStorage storage(kContactStoreV1RegionEnd, fill);
    ContactStoreV1 store(storage, secret_of(1));
    require(store.load() == ContactStoreState::empty,
            "a blank or foreign region was not empty");
    require(store.entries().empty(), "an empty store has entries");
  }
}

void test_round_trip_and_boundaries() {
  MemoryStorage storage(16384, 0xA5);
  const std::vector<std::byte> before = storage.bytes_;
  ContactStoreV1 store(storage, secret_of(1));
  require(store.load() == ContactStoreState::empty, "fresh store not empty");
  const auto entries = sample(kContactStoreV1MaxContacts);
  store.save(entries);
  require(store.state() == ContactStoreState::loaded && store.entries() == entries,
          "the saved state was not loaded");

  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::loaded,
          "a saved store did not load");
  require(reopened.entries() == entries, "entries did not round trip");

  require(std::equal(before.begin(), before.begin() + kContactStoreV1RegionOffset,
                     storage.bytes_.begin()),
          "bytes below the region changed");
  require(std::equal(before.begin() + kContactStoreV1RegionEnd, before.end(),
                     storage.bytes_.begin() + kContactStoreV1RegionEnd),
          "bytes above the region changed");
  for (const auto& [offset, size] : storage.writes) {
    require(size == kContactStoreV1CopySize &&
                (offset == kContactStoreV1CopyOffsets[0] ||
                 offset == kContactStoreV1CopyOffsets[1]),
            "a write was not exactly one copy");
  }

  ContactStoreV1 other(storage, secret_of(2));
  require(other.load() == ContactStoreState::empty,
          "another root secret read the list");
}

void test_saves_alternate_copies() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  for (std::size_t i = 1; i <= 4; ++i) store.save(sample(i));
  require(storage.writes.size() == 4, "unexpected write count");
  for (std::size_t i = 0; i < 4; ++i) {
    require(storage.writes[i].first == kContactStoreV1CopyOffsets[i % 2],
            "saves did not alternate copies");
  }
  ContactStoreV1 reopened(storage, secret_of(1));
  static_cast<void>(reopened.load());
  require(reopened.entries() == sample(4), "the newest save was not selected");
}

void test_interrupted_save_keeps_previous_list() {
  for (const std::size_t tear :
       {std::size_t{0}, std::size_t{1}, std::size_t{12}, std::size_t{63},
        std::size_t{64}, std::size_t{100}, std::size_t{3000},
        kContactStoreV1CopySize - 1}) {
    MemoryStorage storage(kContactStoreV1RegionEnd, 0);
    ContactStoreV1 store(storage, secret_of(1));
    static_cast<void>(store.load());
    store.save(sample(2));
    store.save(sample(3));

    storage.tear_after = tear;
    require(throws([&] { store.save(sample(5)); }),
            "an interrupted save reported success");
    require(store.state() == ContactStoreState::loaded &&
                store.entries() == sample(3),
            "an interrupted save lost the previous list");
    storage.tear_after.reset();

    ContactStoreV1 reopened(storage, secret_of(1));
    require(reopened.load() == ContactStoreState::loaded &&
                reopened.entries() == sample(3),
            "a reopened store lost the previous list");
    reopened.save(sample(6));
    ContactStoreV1 again(storage, secret_of(1));
    require(again.load() == ContactStoreState::loaded &&
                again.entries() == sample(6),
            "a save after an interruption did not win");
  }
}

void test_interrupted_save_after_reload_keeps_previous_list() {
  // The copy chosen after reading the card, not only after a save in the same
  // session, must be the older one.
  for (const std::size_t saves : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
    MemoryStorage storage(kContactStoreV1RegionEnd, 0);
    {
      ContactStoreV1 store(storage, secret_of(1));
      static_cast<void>(store.load());
      for (std::size_t i = 1; i <= saves; ++i) store.save(sample(i));
    }
    ContactStoreV1 reopened(storage, secret_of(1));
    static_cast<void>(reopened.load());
    storage.tear_after = std::size_t{100};
    require(throws([&] { reopened.save(sample(9)); }),
            "an interrupted save reported success");
    storage.tear_after.reset();
    ContactStoreV1 again(storage, secret_of(1));
    require(again.load() == ContactStoreState::loaded &&
                again.entries() == sample(saves),
            "an interrupted save after a reload overwrote the newest copy");
  }
}

void test_lost_write_is_detected() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  store.save(sample(1));
  storage.drop_writes = true;
  require(throws([&] { store.save(sample(2)); }),
          "a write that never reached the card reported success");
  require(store.entries() == sample(1), "the previous list was not kept");
}

void test_relocated_copy_does_not_authenticate() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  store.save(sample(2));  // Copy A.
  // Copy A moved into the B position must not pass as copy B.
  std::copy_n(storage.bytes_.begin() + kContactStoreV1CopyOffsets[0],
              kContactStoreV1CopySize,
              storage.bytes_.begin() + kContactStoreV1CopyOffsets[1]);
  std::fill_n(storage.bytes_.begin() + kContactStoreV1CopyOffsets[0],
              kContactStoreV1CopySize, std::byte{});
  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::empty,
          "a relocated copy authenticated");
}

void test_mixed_descriptor_and_body_is_damaged() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  store.save(sample(1));  // A, generation 1.
  store.save(sample(2));  // B, generation 2.
  store.save(sample(3));  // A, generation 3.
  // Splice an older body under A's newer descriptor: B still intact.
  const SecureBuffer key = detail::derive_store_key(secret_of(1));
  detail::CopyHeader header;
  header.copy_index = 0;
  header.generation = 3;
  header.entry_count = 3;
  const auto fresh = detail::seal_copy(key, kContactStoreV1CopyOffsets[0],
                                       header, detail::encode_body(sample(3)).bytes());
  std::copy_n(fresh.begin(), detail::kDescriptorSize,
              storage.bytes_.begin() + kContactStoreV1CopyOffsets[0]);
  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::loaded &&
              reopened.entries() == sample(2),
          "a spliced copy was not rejected in favour of the intact one");

  // With both copies spliced nothing is intact.
  std::fill_n(storage.bytes_.begin() + kContactStoreV1CopyOffsets[1] +
                  detail::kDescriptorSize + 20,
              4, std::byte{0x77});
  ContactStoreV1 broken(storage, secret_of(1));
  require(broken.load() == ContactStoreState::damaged,
          "two damaged copies were not reported as damaged");
  require(throws([&] { broken.save(sample(1)); }),
          "a damaged store accepted a normal save");
  broken.discard_and_clear();
  ContactStoreV1 recovered(storage, secret_of(1));
  require(recovered.load() == ContactStoreState::loaded &&
              recovered.entries().empty(),
          "discarding did not leave an empty readable list");
  recovered.save(sample(1));
  ContactStoreV1 later(storage, secret_of(1));
  require(later.load() == ContactStoreState::loaded &&
              later.entries() == sample(1),
          "a save after recovery did not win over damaged copies");
}

void place(MemoryStorage& storage, const std::size_t index,
           const detail::CopyHeader& header,
           const std::vector<ContactEntry>& entries) {
  const SecureBuffer key = detail::derive_store_key(secret_of(1));
  const auto copy = detail::seal_copy(key, kContactStoreV1CopyOffsets[index],
                                      header,
                                      detail::encode_body(entries).bytes());
  std::copy(copy.begin(), copy.end(),
            storage.bytes_.begin() + kContactStoreV1CopyOffsets[index]);
}

void test_future_format_is_refused() {
  for (const auto& [version, layout] :
       {std::pair<std::uint16_t, std::uint8_t>{std::uint16_t{2}, detail::kLayoutTwoCopies},
        std::pair<std::uint16_t, std::uint8_t>{std::uint16_t{1}, detail::kLayoutSingleCopy}}) {
    MemoryStorage storage(kContactStoreV1RegionEnd, 0);
    ContactStoreV1 seed(storage, secret_of(1));
    static_cast<void>(seed.load());
    seed.save(sample(1));  // A intact.
    seed.save(sample(2));  // B intact.
    detail::CopyHeader header;
    header.format_version = version;
    header.layout = layout;
    header.copy_index = 0;
    header.generation = 9;
    place(storage, 0, header, {});
    const std::vector<std::byte> before = storage.bytes_;

    ContactStoreV1 store(storage, secret_of(1));
    require(store.load() == ContactStoreState::unsupported_version,
            "a future format was not refused");
    require(store.entries().empty(), "a refused store exposed entries");
    require(throws([&] { store.save(sample(1)); }) &&
                throws([&] { store.discard_and_clear(); }),
            "a refused store accepted a write");
    require(storage.bytes_ == before, "a refused store changed the card");
  }
}

void test_same_generation_conflict_is_inconsistent() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  detail::CopyHeader a;
  a.copy_index = 0;
  a.generation = 4;
  a.entry_count = 1;
  detail::CopyHeader b = a;
  b.copy_index = 1;
  place(storage, 0, a, sample(1));
  place(storage, 1, b, {contact(50, "other")});
  ContactStoreV1 store(storage, secret_of(1));
  require(store.load() == ContactStoreState::inconsistent,
          "a same-generation conflict was not inconsistent");
  store.discard_and_clear();
  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::loaded &&
              reopened.entries().empty(),
          "discarding a conflict did not win");

  // Identical same-generation copies are simply loaded.
  MemoryStorage twin(kContactStoreV1RegionEnd, 0);
  b.entry_count = 1;
  place(twin, 0, a, sample(1));
  place(twin, 1, b, sample(1));
  ContactStoreV1 twins(twin, secret_of(1));
  require(twins.load() == ContactStoreState::loaded &&
              twins.entries() == sample(1),
          "identical copies were not loaded");
}

void test_authenticated_but_invalid_entries_are_damaged() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  detail::CopyHeader header;
  header.copy_index = 0;
  header.generation = 1;
  header.entry_count = 1;  // But two entries encoded: trailing data.
  place(storage, 0, header, sample(2));
  ContactStoreV1 store(storage, secret_of(1));
  require(store.load() == ContactStoreState::damaged,
          "data beyond the entry count was accepted");

  header.entry_count = 2;
  const auto key = sample(1).front().public_key;
  place(storage, 0, header, {ContactEntry{key, "a"}, ContactEntry{key, "b"}});
  require(store.load() == ContactStoreState::damaged,
          "duplicate stored keys were accepted");

  header.entry_count = 49;
  place(storage, 0, header, {});
  require(store.load() == ContactStoreState::damaged,
          "an entry count above the limit was accepted");
}

void test_remark_rules() {
  const auto ok = normalize_contact_remark("  \xE3\x80\x80张三\t ");
  require(ok.remark && *ok.remark == "张三", "blanks were not trimmed");
  require(normalize_contact_remark("一二三四五六七八九十").remark.has_value(),
          "ten CJK characters (30 bytes) were rejected");
  require(normalize_contact_remark("一二三四五六七八九十一").error ==
              ContactRemarkError::too_long,
          "eleven CJK characters were accepted");
  require(normalize_contact_remark(std::string(31, 'a')).remark.has_value(),
          "31 ASCII characters were rejected");
  require(normalize_contact_remark(std::string(32, 'a')).error ==
              ContactRemarkError::too_long,
          "32 ASCII characters were accepted");
  require(normalize_contact_remark(" \xE3\x80\x80 ").error ==
              ContactRemarkError::empty,
          "a blank remark was not empty");
  for (const char* const bad :
       {"a\x01" "b", "a\x7F", "\xC0\xAF", "\xED\xA0\x80", "ab\xE2\x80\xAE" "cd",
        "x\xE2\x80\x8By", "\xEF\xBB\xBFname", "\xF4\x90\x80\x80", "\xE4\xB8"}) {
    require(normalize_contact_remark(bad).error ==
                ContactRemarkError::invalid_text,
            "an invalid remark was accepted");
  }
}

void test_entry_validation() {
  require(validate_contact_entries(sample(kContactStoreV1MaxContacts)) ==
              ContactEntriesError::none,
          "48 contacts were rejected");
  require(validate_contact_entries(sample(kContactStoreV1MaxContacts + 1)) ==
              ContactEntriesError::too_many,
          "49 contacts were accepted");
  auto entries = sample(2);
  entries[1].public_key = entries[0].public_key;
  require(validate_contact_entries(entries) == ContactEntriesError::duplicate_key,
          "a duplicate key was accepted");
  entries = sample(1);
  entries[0].remark = " padded";
  require(validate_contact_entries(entries) ==
              ContactEntriesError::invalid_remark,
          "an unnormalized remark was accepted");
  entries = sample(1);
  entries[0].public_key = {};
  require(validate_contact_entries(entries) == ContactEntriesError::unusable_key,
          "an unusable key was accepted");

  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  require(throws([&] { store.save(entries); }) && storage.writes.empty(),
          "an invalid list was written");
}

void test_stale_view_never_overwrites_newer_copies() {
  // A second card with the same S, or another writer, moved the list on after
  // this store loaded it.
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 stale(storage, secret_of(1));
  static_cast<void>(stale.load());
  stale.save(sample(1));
  {
    ContactStoreV1 other(storage, secret_of(1));
    static_cast<void>(other.load());
    other.save(sample(5));
    other.save(sample(6));
  }
  const std::vector<std::byte> before = storage.bytes_;
  bool changed = false;
  try {
    stale.save(sample(2));
  } catch (const ContactStoreChangedError&) {
    changed = true;
  }
  require(changed, "a stale view saved without ContactStoreChangedError");
  require(storage.bytes_ == before, "a stale view wrote to the card");
  require(stale.state() == ContactStoreState::loaded &&
              stale.entries() == sample(6),
          "the stale view was not reloaded");
  stale.save(sample(7));
  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::loaded &&
              reopened.entries() == sample(7),
          "a save after reloading did not win");
}

void test_misread_load_cannot_overwrite_list() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  {
    ContactStoreV1 seed(storage, secret_of(1));
    static_cast<void>(seed.load());
    seed.save(sample(3));
    seed.save(sample(4));
  }
  ContactStoreV1 store(storage, secret_of(1));
  storage.garble_next_read = true;
  require(store.load() == ContactStoreState::empty,
          "a garbled read did not look empty");
  const std::vector<std::byte> before = storage.bytes_;
  require(throws([&] { store.save(sample(1)); }),
          "a save after a misread succeeded");
  require(storage.bytes_ == before, "a save after a misread wrote the card");
  require(store.entries() == sample(4), "the real list was not reloaded");
}

void test_failures_leave_state_unread() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  storage.fail_reads = true;
  require(throws([&] { static_cast<void>(store.load()); }),
          "a failing read did not throw");
  require(store.state() == ContactStoreState::unread,
          "a failed load did not leave the state unread");
  storage.fail_reads = false;
  static_cast<void>(store.load());
  store.save(sample(1));

  storage.tear_after = std::size_t{10};
  storage.fail_reads_after_tear = true;
  require(throws([&] { store.save(sample(2)); }), "a torn save succeeded");
  require(store.state() == ContactStoreState::unread && store.entries().empty(),
          "a failed reload after a torn save did not leave the state unread");
  require(throws([&] { store.save(sample(2)); }),
          "an unread store accepted a save");
}

void test_resaving_own_entries() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  store.save(sample(3));
  store.save(store.entries());
  ContactStoreV1 reopened(storage, secret_of(1));
  require(reopened.load() == ContactStoreState::loaded &&
              reopened.entries() == sample(3),
          "re-saving the store's own entries changed them");
}

void test_unsupported_second_copy_is_refused() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 seed(storage, secret_of(1));
  static_cast<void>(seed.load());
  seed.save(sample(1));
  detail::CopyHeader header;
  header.format_version = 2;
  header.copy_index = 1;
  header.generation = 2;
  place(storage, 1, header, {});
  ContactStoreV1 store(storage, secret_of(1));
  require(store.load() == ContactStoreState::unsupported_version,
          "an unknown second copy was not refused");
}

void test_descriptor_field_errors_are_damaged() {
  for (int variant = 0; variant < 3; ++variant) {
    MemoryStorage storage(kContactStoreV1RegionEnd, 0);
    detail::CopyHeader header;
    header.copy_index = 0;
    header.generation = 1;
    header.entry_count = 1;
    if (variant == 0) header.copy_index = 1;
    if (variant == 1) header.body_plain_size = 3099;
    if (variant == 2) header.reserved_probe = 1;
    place(storage, 0, header, sample(1));
    ContactStoreV1 store(storage, secret_of(1));
    require(store.load() == ContactStoreState::damaged,
            "a descriptor with a wrong field was not damaged");
  }
}

void test_copies_never_repeat_bytes() {
  MemoryStorage storage(kContactStoreV1RegionEnd, 0);
  ContactStoreV1 store(storage, secret_of(1));
  static_cast<void>(store.load());
  const auto copy_a = [&] {
    return std::vector<std::byte>(
        storage.bytes_.begin() + kContactStoreV1CopyOffsets[0],
        storage.bytes_.begin() + kContactStoreV1CopyOffsets[0] +
            kContactStoreV1CopySize);
  };
  store.save(sample(2));
  const auto first = copy_a();
  store.save(sample(2));
  store.save(sample(2));  // Same contents, copy A again.
  const auto second = copy_a();
  require(!std::equal(first.begin(), first.begin() + detail::kNonceSize,
                      second.begin()),
          "a descriptor nonce repeated");
  const auto body = detail::kDescriptorSize;
  require(!std::equal(first.begin() + body,
                      first.begin() + body + detail::kNonceSize,
                      second.begin() + body),
          "a body nonce repeated");
  std::size_t equal = 0;
  for (std::size_t i = 0; i < first.size(); ++i) equal += first[i] == second[i];
  require(equal < first.size() / 16,
          "two copies of the same list share many byte positions");
}

void test_invisible_remarks_are_rejected() {
  for (const char* const bad :
       {"\xE3\x85\xA4", "a\xEF\xB8\x8F", "\xE2\xA0\x80", "\xEF\xBE\xA0"}) {
    require(normalize_contact_remark(bad).error ==
                ContactRemarkError::invalid_text,
            "an invisible remark was accepted");
  }
  const auto trimmed =
      normalize_contact_remark("\xC2\xA0\xE2\x80\x83Li\xE2\x80\xAF");
  require(trimmed.remark && *trimmed.remark == "Li",
          "Unicode white space was not trimmed");
  require(normalize_contact_remark("Li Lei").remark == std::string("Li Lei"),
          "inner spaces were removed");
}

}  // namespace

int main() {
  try {
    test_unavailable_on_small_card();
    test_blank_and_foreign_regions_are_empty();
    test_round_trip_and_boundaries();
    test_saves_alternate_copies();
    test_interrupted_save_keeps_previous_list();
    test_interrupted_save_after_reload_keeps_previous_list();
    test_lost_write_is_detected();
    test_relocated_copy_does_not_authenticate();
    test_mixed_descriptor_and_body_is_damaged();
    test_future_format_is_refused();
    test_same_generation_conflict_is_inconsistent();
    test_authenticated_but_invalid_entries_are_damaged();
    test_remark_rules();
    test_entry_validation();
    test_stale_view_never_overwrites_newer_copies();
    test_misread_load_cannot_overwrite_list();
    test_failures_leave_state_unread();
    test_resaving_own_entries();
    test_unsupported_second_copy_is_refused();
    test_descriptor_field_errors_are_damaged();
    test_copies_never_repeat_bytes();
    test_invisible_remarks_are_rejected();
  } catch (const std::exception& error) {
    std::cerr << "contact_store_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "contact_store_v1_tests passed\n";
  return 0;
}
