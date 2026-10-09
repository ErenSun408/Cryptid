#include "card/card_record_journal_v2.h"

#include "card/card_record_crypto.h"
#include "card/card_record_v2_internal.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v2.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <system_error>
#include <utility>

namespace usb_vault::card {
namespace {

using Id = std::array<std::byte, 16>;
using Digest = std::array<std::byte, 32>;
using Descriptor = std::array<std::byte, 152>;
using detail::card_journal_u32;
using detail::card_journal_store_u32;
constexpr std::array<std::byte, 8> kDescriptorMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'J'}, std::byte{'R'},
    std::byte{'N'}, std::byte{'2'}, std::byte{}, std::byte{}};
static_assert(kCardRecordJournalV2Slot0Offset ==
    kCardRecordJournalV2Descriptor1Offset + 152);
static_assert(kCardRecordJournalV2Slot1Offset ==
    kCardRecordJournalV2Slot0Offset + 384);
static_assert(kCardRecordJournalV2StorageSize ==
    kCardRecordJournalV2Slot1Offset + 384);

[[noreturn]] void fail(const CardError error, const char* message) {
  throw std::system_error(make_error_code(error), message);
}

bool equal(const std::span<const std::byte> a,
    const std::span<const std::byte> b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned int difference = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    difference |= std::to_integer<unsigned int>(a[i] ^ b[i]);
  }
  return difference == 0;
}

void require_capacity(ICardRecordJournalStorage& storage) {
  if (storage.capacity() < kCardRecordJournalV2StorageSize) {
    fail(CardError::invalid_configuration, "card storage too small for v2 journal");
  }
}

security::SecureBuffer read_exact(ICardRecordJournalStorage& storage,
    const std::size_t offset, const std::size_t length) {
  auto result = storage.read(offset, length);
  if (result.size() != length) fail(CardError::device_failure, "short v2 journal read");
  return result;
}

void write_verified(ICardRecordJournalStorage& storage, const std::size_t offset,
    const std::span<const std::byte> bytes) {
  std::exception_ptr failure;
  try { storage.write(offset, bytes); }
  catch (...) { failure = std::current_exception(); }
  auto observed = read_exact(storage, offset, bytes.size());
  if (equal(observed.bytes(), bytes)) return;
  if (failure) std::rethrow_exception(failure);
  fail(CardError::device_failure, "v2 journal writeback mismatch");
}

Digest sha256(const std::span<const std::byte> bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  auto status = ::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
      nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
        "open v2 journal hash provider");
  }
  Digest digest{};
  status = ::BCryptHash(algorithm, nullptr, 0,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()));
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
        "hash v2 journal bytes");
  }
  return digest;
}

Id random_id() {
  Id id{};
  detail::fill_card_random(id);
  return id;
}

Id distinct_id(const Id& first, const Id& second) {
  for (;;) {
    auto candidate = random_id();
    if (!equal(candidate, first) && !equal(candidate, second)) return candidate;
  }
}

std::size_t slot_offset(const std::uint32_t slot) noexcept {
  return slot == 0 ? kCardRecordJournalV2Slot0Offset : kCardRecordJournalV2Slot1Offset;
}

std::size_t descriptor_offset(const std::uint32_t descriptor) noexcept {
  return descriptor == 0 ? kCardRecordJournalV2Descriptor0Offset :
      kCardRecordJournalV2Descriptor1Offset;
}

Descriptor make_descriptor(const Id& commit, const Id& previous,
    const Id& instance, const std::uint32_t slot,
    const std::span<const std::byte> record,
    const std::span<const std::byte> inactive) {
  Descriptor result{};
  std::ranges::copy(kDescriptorMagic, result.begin());
  card_journal_store_u32(result, 8, 2);
  card_journal_store_u32(result, 12, 152);
  std::ranges::copy(commit, result.begin() + 16);
  std::ranges::copy(previous, result.begin() + 32);
  card_journal_store_u32(result, 48, slot);
  card_journal_store_u32(result, 52, 384);
  const auto record_digest = sha256(record);
  std::ranges::copy(record_digest, result.begin() + 56);
  std::ranges::copy(instance, result.begin() + 88);
  const auto inactive_digest = sha256(inactive);
  std::ranges::copy_n(inactive_digest.begin(), 16, result.begin() + 104);
  const auto descriptor_digest = sha256(std::span(result).first(120));
  std::ranges::copy(descriptor_digest, result.begin() + 120);
  return result;
}

struct Parsed final {
  bool valid{};
  bool record_matches{};
  bool inactive_matches{};
  Id commit{};
  Id previous{};
  Id instance{};
  std::uint32_t slot{};
};

Parsed parse_descriptor(const std::span<const std::byte> descriptor,
    const std::array<Digest, 2>& digests) {
  if (!equal(descriptor.first(8), kDescriptorMagic) ||
      !equal(descriptor.last(32), sha256(descriptor.first(120)))) return {};
  if (card_journal_u32(descriptor, 8) != 2 ||
      card_journal_u32(descriptor, 12) != 152 ||
      card_journal_u32(descriptor, 52) != 384) {
    fail(CardError::unsupported, "unsupported complete v2 journal descriptor");
  }
  const auto slot = card_journal_u32(descriptor, 48);
  if (slot > 1) fail(CardError::device_failure, "invalid v2 journal slot");
  Parsed result{.valid = true, .slot = slot};
  std::ranges::copy(descriptor.subspan(16, 16), result.commit.begin());
  std::ranges::copy(descriptor.subspan(32, 16), result.previous.begin());
  std::ranges::copy(descriptor.subspan(88, 16), result.instance.begin());
  if (equal(result.commit, result.previous)) {
    fail(CardError::device_failure, "invalid v2 journal commit identity");
  }
  result.record_matches = equal(descriptor.subspan(56, 32), digests[slot]);
  result.inactive_matches = equal(descriptor.subspan(104, 16),
      std::span(digests[slot ^ 1U]).first(16));
  return result;
}

struct Extension final {
  security::SecureBuffer bytes;
  std::array<Parsed, 2> parsed{};
};

Extension read_extension(ICardRecordJournalStorage& storage) {
  require_capacity(storage);
  Extension result{.bytes = read_exact(storage, detail::kCardV2ExtensionBase,
      detail::kCardV2ExtensionSize)};
  const std::array<Digest, 2> digests{
      sha256(result.bytes.bytes().subspan(304, 384)),
      sha256(result.bytes.bytes().subspan(688, 384))};
  for (std::size_t i = 0; i < 2; ++i) {
    result.parsed[i] = parse_descriptor(result.bytes.bytes().subspan(i * 152, 152),
        digests);
  }
  return result;
}

struct Loaded final {
  security::SecureBuffer record;
  Id commit{};
  Id previous{};
  Id instance{};
  std::uint32_t slot{};
  std::uint32_t descriptor{};
  bool mirrored{};
};

Loaded load_extension(ICardRecordJournalStorage& storage, const Id& instance) {
  auto extension = read_extension(storage);
  const auto& parsed = extension.parsed;
  for (const auto& descriptor : parsed) {
    if (descriptor.valid && !equal(descriptor.instance, instance)) {
      fail(CardError::device_failure, "v2 journal instance conflict");
    }
  }
  std::uint32_t selected = 0;
  if (!parsed[0].valid && !parsed[1].valid) {
    fail(CardError::device_failure, "v2 journal has no recoverable commit");
  }
  if (!parsed[0].valid) {
    selected = 1;
  } else if (parsed[1].valid && !equal(extension.bytes.bytes().first(152),
      extension.bytes.bytes().subspan(152, 152))) {
    const bool same_commit = equal(parsed[0].commit, parsed[1].commit) &&
        equal(parsed[0].previous, parsed[1].previous) &&
        parsed[0].slot == parsed[1].slot;
    if (same_commit) {
      if (parsed[0].inactive_matches == parsed[1].inactive_matches) {
        fail(CardError::device_failure, "ambiguous v2 mirrored metadata");
      }
      selected = parsed[1].inactive_matches ? 1U : 0U;
    } else {
      const bool zero_follows_one = equal(parsed[0].previous, parsed[1].commit);
      const bool one_follows_zero = equal(parsed[1].previous, parsed[0].commit);
      if (zero_follows_one == one_follows_zero || parsed[0].slot == parsed[1].slot) {
        fail(CardError::device_failure, "ambiguous v2 commit chain");
      }
      selected = one_follows_zero ? 1U : 0U;
    }
  }
  const auto& chosen = parsed[selected];
  if (!chosen.record_matches) {
    fail(CardError::device_failure, "active v2 record is damaged; rollback refused");
  }
  security::SecureBuffer record(384);
  std::ranges::copy(extension.bytes.bytes().subspan(304 + chosen.slot * 384, 384),
      record.bytes().begin());
  if (inspect_card_record_v2(record.bytes()) != CardRecordV2State::initialized) {
    fail(CardError::unsupported, "selected v2 logical record is unsupported or damaged");
  }
  return {.record = std::move(record), .commit = chosen.commit,
      .previous = chosen.previous, .instance = chosen.instance, .slot = chosen.slot,
      .descriptor = selected, .mirrored = parsed[0].valid && parsed[1].valid &&
          equal(extension.bytes.bytes().first(152),
              extension.bytes.bytes().subspan(152, 152)) && chosen.inactive_matches};
}

struct CardSnapshot final {
  security::SecureBuffer anchor;
  Loaded journal;
};

CardSnapshot load_card(ICardRecordJournalStorage& storage) {
  require_capacity(storage);
  auto legacy = snapshot_card_record_journal_v1(storage);
  if (!detail::is_card_record_v2_anchor(legacy.bytes())) {
    fail(CardError::unsupported, "card has no supported v2 anchor");
  }
  Id instance{};
  std::ranges::copy(legacy.bytes().subspan(48, 16), instance.begin());
  auto journal = load_extension(storage, instance);
  if (!equal(legacy.bytes().subspan(32, 16),
      journal.record.bytes().subspan(64, 16))) {
    fail(CardError::device_failure, "v2 anchor record identity conflict");
  }
  return {.anchor = std::move(legacy), .journal = std::move(journal)};
}

void normalize_extension(ICardRecordJournalStorage& storage, const Loaded& loaded) {
  if (loaded.mirrored) return;
  security::SecureBuffer filler(384);
  detail::fill_card_random(filler.bytes());
  write_verified(storage, slot_offset(loaded.slot ^ 1U), filler.bytes());
  auto descriptor = make_descriptor(loaded.commit, loaded.previous, loaded.instance,
      loaded.slot, loaded.record.bytes(), filler.bytes());
  write_verified(storage, descriptor_offset(loaded.descriptor ^ 1U), descriptor);
  write_verified(storage, descriptor_offset(loaded.descriptor), descriptor);
  auto observed = load_extension(storage, loaded.instance);
  if (!observed.mirrored || !equal(observed.commit, loaded.commit) ||
      !equal(observed.record.bytes(), loaded.record.bytes())) {
    fail(CardError::device_failure, "v2 journal normalization failed");
  }
}

security::SecureBuffer anchor_mac(const security::SecureBuffer& root,
    const std::span<const std::byte> record, const std::span<const std::byte> anchor) {
  auto key = detail::derive_card_record_domain_key(root, record.subspan(32, 64),
      "usb-vault:card-journal-anchor:v2");
  return detail::card_hmac_sha256(key, anchor.first(detail::kCardV2AnchorTagOffset));
}

void require_anchor_root(const CardSnapshot& snapshot,
    const security::SecureBuffer& root) {
  auto mac = anchor_mac(root, snapshot.journal.record.bytes(), snapshot.anchor.bytes());
  if (!equal(mac.bytes(), snapshot.anchor.bytes().last(32))) throw CardCredentialRejected();
}

std::uint32_t ring_cursor(const std::span<const std::byte> record,
    const security::SecureBuffer& root) {
  auto key = detail::derive_card_ring_metadata_key_v2(root, record.subspan(32, 64));
  auto metadata = detail::open_card_aead(key, record.subspan(324, 60), record.first(324));
  const auto version = card_journal_u32(metadata.plaintext.bytes(), 0);
  const auto cursor = card_journal_u32(metadata.plaintext.bytes(), 4);
  metadata.plaintext.clear();
  if (!metadata.authenticated || version != 1 || cursor >= 3) throw CardCredentialRejected();
  return cursor;
}

void require_append_transition(const std::span<const std::byte> expected,
    const std::span<const std::byte> replacement,
    const security::SecureBuffer& root) {
  if (inspect_card_record_v2(expected) != CardRecordV2State::initialized ||
      inspect_card_record_v2(replacement) != CardRecordV2State::initialized ||
      !equal(expected.first(96), replacement.first(96))) {
    fail(CardError::invalid_configuration, "invalid v2 append framing");
  }
  unsigned int changed = 0;
  std::size_t changed_slot = 0;
  for (std::size_t i = 0; i < 3; ++i) {
    if (!equal(expected.subspan(96 + i * 76, 76), replacement.subspan(96 + i * 76, 76))) {
      ++changed;
      changed_slot = i;
    }
  }
  const auto cursor = ring_cursor(expected, root);
  if (changed_slot != cursor || ring_cursor(replacement, root) != detail::next_card_slot_v2(cursor)) {
    fail(CardError::invalid_configuration, "v2 append did not follow authenticated cursor");
  }
  if (changed != 1 || equal(expected.subspan(324, 60), replacement.subspan(324, 60))) {
    fail(CardError::invalid_configuration, "v2 append must replace exactly one envelope");
  }
}

} // namespace

security::SecureBuffer snapshot_card_record_journal_v2(ICardRecordJournalStorage& storage) {
  return std::move(load_card(storage).journal.record);
}

void verify_card_record_journal_v2_anchor(ICardRecordJournalStorage& storage,
    const std::span<const std::byte> expected_record,
    const security::SecureBuffer& authenticated_root) {
  auto snapshot = load_card(storage);
  if (!equal(snapshot.journal.record.bytes(), expected_record)) {
    fail(CardError::invalid_operation, "v2 anchor verification snapshot changed");
  }
  require_anchor_root(snapshot, authenticated_root);
}

void recover_card_record_journal_v2(ICardRecordJournalStorage& storage,
    const std::span<const std::byte> expected_record,
    const security::SecureBuffer& authenticated_root) {
  auto snapshot = load_card(storage);
  if (!equal(snapshot.journal.record.bytes(), expected_record)) {
    fail(CardError::invalid_operation, "v2 recovery snapshot changed");
  }
  require_anchor_root(snapshot, authenticated_root);
  static_cast<void>(ring_cursor(expected_record, authenticated_root));
  normalize_extension(storage, snapshot.journal);
  // Legacy normalization removes any pre-publication credential copy too.
  auto anchor = read_card_record_journal_v1(storage);
  if (!equal(anchor.bytes(), snapshot.anchor.bytes())) {
    fail(CardError::device_failure, "v2 anchor changed during normalization");
  }
  auto observed = load_card(storage);
  require_anchor_root(observed, authenticated_root);
  if (!observed.journal.mirrored || !equal(observed.journal.record.bytes(), expected_record)) {
    fail(CardError::device_failure, "v2 recovery did not verify");
  }
}

void commit_card_record_journal_v2(ICardRecordJournalStorage& storage,
    const std::span<const std::byte> expected_record,
    const std::span<const std::byte> replacement_record,
    const security::SecureBuffer& authenticated_root) {
  require_append_transition(expected_record, replacement_record, authenticated_root);
  auto snapshot = load_card(storage);
  if (equal(snapshot.journal.record.bytes(), replacement_record)) {
    recover_card_record_journal_v2(storage, replacement_record, authenticated_root);
    return;
  }
  require_anchor_root(snapshot, authenticated_root);
  if (!equal(snapshot.journal.record.bytes(), expected_record)) {
    fail(CardError::invalid_operation, "v2 append snapshot changed");
  }
  recover_card_record_journal_v2(storage, expected_record, authenticated_root);
  const auto& current = snapshot.journal;
  const auto commit = distinct_id(current.commit, current.previous);
  security::SecureBuffer filler(384);
  detail::fill_card_random(filler.bytes());
  write_verified(storage, slot_offset(current.slot ^ 1U), replacement_record);
  auto descriptor = make_descriptor(commit, current.commit, current.instance,
      current.slot ^ 1U, replacement_record, filler.bytes());
  write_verified(storage, descriptor_offset(current.descriptor ^ 1U), descriptor);
  write_verified(storage, slot_offset(current.slot), filler.bytes());
  write_verified(storage, descriptor_offset(current.descriptor), descriptor);
  auto observed = load_card(storage);
  require_anchor_root(observed, authenticated_root);
  if (!observed.journal.mirrored || !equal(observed.journal.commit, commit) ||
      !equal(observed.journal.record.bytes(), replacement_record) ||
      !equal(observed.anchor.bytes(), snapshot.anchor.bytes())) {
    fail(CardError::device_failure, "v2 append did not verify");
  }
}

} // namespace usb_vault::card
