#include "card/card_record_journal_v2.h"
#include "card/vikey/vikey_card_journal_store.h"
#include "card/vikey/card_operation_lease.h"
#include "card/vikey/vikey_helper_card_enrollment.h"
#include "usb_vault/security/confirmed_password.h"

#include "card/card_record_crypto.h"
#include "card/card_record_v2_internal.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v2.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace usb_vault::card;
using usb_vault::security::SecureBuffer;
namespace crypto = usb_vault::card::detail;
namespace security = usb_vault::security;
constexpr std::array<std::string_view, 5> kPasswords{
    "ring-password-one", "ring-password-two", "ring-password-three",
    "ring-password-four", "ring-password-five"};

void expect(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

SecureBuffer copy(const std::span<const std::byte> input) {
  SecureBuffer result(input.size());
  std::ranges::copy(input, result.bytes().begin());
  return result;
}

SecureBuffer text(const std::string_view input) {
  return copy(std::as_bytes(std::span(input.data(), input.size())));
}

bool same(const std::span<const std::byte> a, const std::span<const std::byte> b) {
  return std::ranges::equal(a, b);
}

template <typename Error, typename Action>
void rejects(Action&& action, const char* message) {
  try { action(); }
  catch (const Error&) { return; }
  throw std::runtime_error(message);
}

class Memory final : public ICardRecordJournalStorage {
 public:
  Memory() : data(1808 + 64) {
    std::ranges::fill(std::span(data).subspan(1808), std::byte{0xa5});
  }
  Memory(const Memory& source) : Memory() {
    data = source.data;
    reported_capacity = source.reported_capacity;
  }
  std::size_t capacity() override { return reported_capacity; }
  SecureBuffer read(const std::size_t offset, const std::size_t length) override {
    const auto call = reads++;
    if (offline) throw std::system_error(make_error_code(CardError::communication_failed));
    if (offset > data.size() || length > data.size() - offset) {
      throw std::runtime_error("test read outside storage");
    }
    auto result = copy(std::span(data).subspan(offset, length));
    if (call == short_read) return copy(result.bytes().first(length - 1));
    return result;
  }
  void write(const std::size_t offset, const std::span<const std::byte> bytes) override {
    const auto call = writes++;
    if (offline) throw std::system_error(make_error_code(CardError::communication_failed));
    if (offset > 1808 || bytes.size() > 1808 - offset) {
      throw std::runtime_error("test write outside application region");
    }
    auto length = bytes.size();
    if (call == cut_write) length = tear_mode == 0 ? 0 : bytes.size() / tear_mode;
    std::ranges::copy_n(bytes.begin(), length, data.begin() + offset);
    if (call == cut_write) {
      offline = true; // Includes a readback failure after a complete write.
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
    if (call == report_error_after_write) {
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
  }
  void online() {
    offline = false;
    cut_write = short_read = report_error_after_write = std::numeric_limits<std::size_t>::max();
    reads = writes = 0;
  }
  void sentinel() const {
    expect(std::ranges::all_of(std::span(data).subspan(1808),
        [](const std::byte value) { return value == std::byte{0xa5}; }),
        "transaction changed out-of-region sentinel");
  }
  std::vector<std::byte> data;
  std::size_t reported_capacity{1808 + 64};
  std::size_t reads{}, writes{};
  std::size_t cut_write{std::numeric_limits<std::size_t>::max()};
  std::size_t short_read{std::numeric_limits<std::size_t>::max()};
  std::size_t report_error_after_write{std::numeric_limits<std::size_t>::max()};
  std::size_t tear_mode{2};
  bool offline{};
};

std::array<std::byte, 32> sha256(const std::span<const std::byte> bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  expect(BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
      nullptr, 0)), "test hash provider unavailable");
  std::array<std::byte, 32> digest{};
  const auto status = ::BCryptHash(algorithm, nullptr, 0,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), reinterpret_cast<PUCHAR>(digest.data()), 32);
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));
  expect(BCRYPT_SUCCESS(status), "test hash failed");
  return digest;
}

using Id = std::array<std::byte, 16>;
Id random_id() {
  Id result{};
  crypto::fill_card_random(result);
  return result;
}

std::array<std::byte, 152> descriptor(const std::uint32_t version,
    const Id& commit, const Id& previous, const std::uint32_t slot,
    const std::span<const std::byte> active, const std::span<const std::byte> inactive,
    const Id& instance) {
  std::array<std::byte, 152> result{};
  constexpr std::string_view magic = "UVJRN2";
  std::ranges::copy(std::as_bytes(std::span(magic.data(), magic.size())), result.begin());
  crypto::card_journal_store_u32(result, 8, version);
  crypto::card_journal_store_u32(result, 12, 152);
  std::ranges::copy(commit, result.begin() + 16);
  std::ranges::copy(previous, result.begin() + 32);
  crypto::card_journal_store_u32(result, 48, slot);
  crypto::card_journal_store_u32(result, 52, static_cast<std::uint32_t>(active.size()));
  std::ranges::copy(sha256(active), result.begin() + 56);
  const auto inactive_hash = sha256(inactive);
  if (version == 1) {
    std::ranges::copy(inactive_hash, result.begin() + 88);
  } else {
    std::ranges::copy(instance, result.begin() + 88);
    std::ranges::copy_n(inactive_hash.begin(), 16, result.begin() + 104);
  }
  std::ranges::copy(sha256(std::span(result).first(120)), result.begin() + 120);
  return result;
}

// Explicit on-media fixture, not a production initialization or migration API.
// It encodes the published schema without invoking the journal writer under test.
void fixture(Memory& storage, const InitializedCardRecordV2& initial) {
  const auto instance = random_id();
  SecureBuffer anchor(216);
  crypto::fill_card_random(anchor.bytes());
  std::ranges::copy(crypto::kCardV2AnchorMagic, anchor.bytes().begin());
  for (const auto [offset, value] : std::array<std::pair<std::size_t, std::uint32_t>, 6>{
      {{8, 2}, {12, 216}, {16, 736}, {20, 1072}, {24, 384}, {28, 1}}}) {
    crypto::card_journal_store_u32(anchor.bytes(), offset, value);
  }
  std::ranges::copy(initial.record.bytes().subspan(64, 16), anchor.bytes().begin() + 32);
  std::ranges::copy(instance, anchor.bytes().begin() + 48);
  auto key = crypto::derive_card_record_domain_key(initial.root_secret,
      initial.record.bytes().subspan(32, 64), "usb-vault:card-journal-anchor:v2");
  auto mac = crypto::card_hmac_sha256(key, anchor.bytes().first(184));
  std::ranges::copy(mac.bytes(), anchor.bytes().begin() + 184);
  crypto::fill_card_random(std::span(storage.data).subspan(520, 216));
  std::ranges::copy(anchor.bytes(), storage.data.begin() + 304);
  const auto legacy = descriptor(1, random_id(), random_id(), 0, anchor.bytes(),
      std::span(storage.data).subspan(520, 216), instance);
  std::ranges::copy(legacy, storage.data.begin());
  std::ranges::copy(legacy, storage.data.begin() + 152);
  crypto::fill_card_random(std::span(storage.data).subspan(1424, 384));
  std::ranges::copy(initial.record.bytes(), storage.data.begin() + 1040);
  const auto extension = descriptor(2, random_id(), random_id(), 0, initial.record.bytes(),
      std::span(storage.data).subspan(1424, 384), instance);
  std::ranges::copy(extension, storage.data.begin() + 736);
  std::ranges::copy(extension, storage.data.begin() + 888);
  storage.online();
  expect(same(snapshot_card_record_journal_v2(storage).bytes(), initial.record.bytes()),
      "fixture does not match the independently encoded schema");
}

void rehash_descriptor(Memory& storage, const std::size_t offset) {
  std::ranges::copy(sha256(std::span(storage.data).subspan(offset, 120)),
      storage.data.begin() + offset + 120);
}

void test_append_faults_and_retry(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  Memory completed = base;
  commit_card_record_journal_v2(completed, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  expect(completed.writes == 4, "append must use four publication/cleanup writes");
  auto found = unlock_card_record_v2(snapshot_card_record_journal_v2(completed).bytes(), text(kPasswords[1]));
  expect(same(found.bytes(), initial.root_secret.bytes()), "append changed root or published unusable credentials");
  for (std::size_t call = 0; call < 4; ++call) {
    for (const std::size_t mode : {0U, 2U, 1U}) {
      Memory interrupted = base;
      interrupted.cut_write = call;
      interrupted.tear_mode = mode;
      rejects<std::system_error>([&] {
        commit_card_record_journal_v2(interrupted, initial.record.bytes(), replacement.bytes(), initial.root_secret);
      }, "power loss did not interrupt append");
      interrupted.online();
      auto selected = snapshot_card_record_journal_v2(interrupted);
      expect(same(selected.bytes(), initial.record.bytes()) || same(selected.bytes(), replacement.bytes()),
          "torn append lost both valid states");
      expect(interrupted.writes == 0, "snapshot performed unauthenticated repair");
      commit_card_record_journal_v2(interrupted, initial.record.bytes(), replacement.bytes(), initial.root_secret);
      expect(same(snapshot_card_record_journal_v2(interrupted).bytes(), replacement.bytes()),
          "retry advanced beyond the original candidate");
      expect(!same(std::span(interrupted.data).subspan(1040, 384), initial.record.bytes()) &&
          !same(std::span(interrupted.data).subspan(1424, 384), initial.record.bytes()),
          "successful append retained the old physical record");
      interrupted.online();
      commit_card_record_journal_v2(interrupted, initial.record.bytes(), replacement.bytes(), initial.root_secret);
      expect(interrupted.writes == 0, "completed retry created another commit");
      interrupted.sentinel();
    }
    Memory reported = base;
    reported.report_error_after_write = call;
    commit_card_record_journal_v2(reported, initial.record.bytes(), replacement.bytes(), initial.root_secret);
    expect(same(snapshot_card_record_journal_v2(reported).bytes(), replacement.bytes()),
        "complete write error was not resolved by exact readback");
  }
  std::cout << "append faults: 12; complete write error checks: 4\n";
}

void test_recovery_faults(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  Memory published = base;
  published.cut_write = 2;
  published.tear_mode = 0;
  rejects<std::system_error>([&] {
    commit_card_record_journal_v2(published, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  }, "missing post-publication interruption");
  published.online();
  Memory legacy_pending = base;
  const auto previous = random_id();
  // A future anchor is authoritative, but a previous old-region copy remains.
  const auto anchor = snapshot_card_record_journal_v1(legacy_pending);
  auto old = copy(initial.record.bytes().subspan(32, 64));
  SecureBuffer old_record(216);
  std::ranges::copy(old.bytes(), old_record.bytes().begin());
  std::ranges::copy(initial.record.bytes().subspan(96, 152), old_record.bytes().begin() + 64);
  std::ranges::copy(old_record.bytes(), legacy_pending.data.begin() + 520);
  const auto newest = descriptor(1, random_id(), previous, 0, anchor.bytes(), old_record.bytes(), {});
  const auto obsolete = descriptor(1, previous, random_id(), 1, old_record.bytes(), anchor.bytes(), {});
  std::ranges::copy(newest, legacy_pending.data.begin());
  std::ranges::copy(obsolete, legacy_pending.data.begin() + 152);
  for (const Memory* state : {&published, &legacy_pending}) {
    const auto expected = state == &published ? replacement.bytes() : initial.record.bytes();
    for (std::size_t call = 0; call < 3; ++call) {
      for (const std::size_t mode : {0U, 2U, 1U}) {
        Memory interrupted = *state;
        interrupted.cut_write = call;
        interrupted.tear_mode = mode;
        rejects<std::system_error>([&] {
          recover_card_record_journal_v2(interrupted, expected, initial.root_secret);
        }, "cleanup power loss did not interrupt recovery");
        interrupted.online();
        expect(same(snapshot_card_record_journal_v2(interrupted).bytes(), expected),
            "cleanup power loss rolled back published credentials");
        expect(interrupted.writes == 0, "snapshot performed repair after power loss");
        recover_card_record_journal_v2(interrupted, expected, initial.root_secret);
        if (state == &legacy_pending) {
          expect(!same(std::span(interrupted.data).subspan(304, 216), old_record.bytes()) &&
              !same(std::span(interrupted.data).subspan(520, 216), old_record.bytes()),
              "recovery retained an addressable legacy credential copy");
        }
        interrupted.online();
        recover_card_record_journal_v2(interrupted, expected, initial.root_secret);
        expect(interrupted.writes == 0, "healthy recovery wrote storage");
        interrupted.sentinel();
      }
    }
  }
  legacy_pending.online();
  commit_card_record_journal_v2(legacy_pending, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  expect(legacy_pending.writes == 7 &&
      !same(std::span(legacy_pending.data).subspan(520, 216), old_record.bytes()),
      "append did not finish pending legacy cleanup before reporting success");
  std::cout << "extension/legacy cleanup recovery faults: 18\n";
}

void test_authentication_and_transition_rejection(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  Memory bad_mac = base;
  bad_mac.data[304 + 70] ^= std::byte{1};
  for (const auto offset : {0U, 152U}) {
    std::ranges::copy(sha256(std::span(bad_mac.data).subspan(304, 216)), bad_mac.data.begin() + offset + 56);
    rehash_descriptor(bad_mac, offset);
  }
  bad_mac.data[736 + 120] ^= std::byte{1}; // Otherwise recoverable extension.
  rejects<CardCredentialRejected>([&] {
    recover_card_record_journal_v2(bad_mac, initial.record.bytes(), initial.root_secret);
  }, "forged anchor triggered repair writes");
  rejects<CardCredentialRejected>([&] {
    commit_card_record_journal_v2(bad_mac, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  }, "forged anchor permitted append");
  expect(bad_mac.writes == 0, "anchor authentication failure wrote storage");
  SecureBuffer wrong_root(32);
  crypto::fill_card_random(wrong_root.bytes());
  Memory denied = base;
  rejects<CardCredentialRejected>([&] {
    recover_card_record_journal_v2(denied, initial.record.bytes(), wrong_root);
  }, "wrong root permitted repair");
  auto invalid = copy(replacement.bytes());
  invalid.bytes().back() ^= std::byte{1};
  rejects<CardCredentialRejected>([&] {
    commit_card_record_journal_v2(denied, initial.record.bytes(), invalid.bytes(), initial.root_secret);
  }, "unauthenticated candidate metadata permitted append");
  const auto cursor = crypto::authenticate_card_record_v2(initial.record.bytes(), text(kPasswords[0])).next_slot;
  crypto::seal_card_ring_metadata_v2(invalid.bytes(), initial.root_secret, cursor);
  rejects<std::system_error>([&] {
    commit_card_record_journal_v2(denied, initial.record.bytes(), invalid.bytes(), initial.root_secret);
  }, "candidate failed to advance its cursor but was accepted");
  expect(denied.writes == 0, "invalid root/transition wrote storage");
  denied.sentinel();
}

void test_corruption_stale_snapshots_and_limits(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  Memory damaged = base;
  damaged.cut_write = 2;
  damaged.tear_mode = 0;
  rejects<std::system_error>([&] {
    commit_card_record_journal_v2(damaged, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  }, "missing newest commit publication");
  damaged.online();
  damaged.data[1424 + 100] ^= std::byte{1};
  rejects<std::system_error>([&] { static_cast<void>(snapshot_card_record_journal_v2(damaged)); },
      "damaged complete newest commit rolled back to available old credentials");
  expect(damaged.writes == 0, "damaged snapshot attempted repair");
  for (const std::size_t mode : {0U, 1U, 2U, 3U}) {
    Memory rejected = base;
    if (mode == 0) rejected.reported_capacity = 1807;
    if (mode == 1) rejected.short_read = 0;
    if (mode == 2) {
      crypto::card_journal_store_u32(std::span(rejected.data), 736 + 8, 9);
      rehash_descriptor(rejected, 736);
    }
    if (mode == 3) {
      rejected.data[888 + 88] ^= std::byte{1};
      rehash_descriptor(rejected, 888);
    }
    auto before = rejected.data;
    rejects<std::system_error>([&] {
      commit_card_record_journal_v2(rejected, initial.record.bytes(), replacement.bytes(), initial.root_secret);
    }, "capacity/short-read/future/instance mismatch permitted append");
    expect(rejected.writes == 0 && rejected.data == before, "rejected card was modified");
    rejected.sentinel();
  }
  Memory stale = base;
  commit_card_record_journal_v2(stale, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  auto third = append_card_password_v2(replacement.bytes(), text(kPasswords[0]), text(kPasswords[2]));
  commit_card_record_journal_v2(stale, replacement.bytes(), third.bytes(), initial.root_secret);
  stale.online();
  rejects<std::system_error>([&] {
    commit_card_record_journal_v2(stale, initial.record.bytes(), replacement.bytes(), initial.root_secret);
  }, "stale candidate was replayed");
  rejects<std::system_error>([&] {
    recover_card_record_journal_v2(stale, initial.record.bytes(), initial.root_secret);
  }, "stale snapshot permitted repair");
  expect(stale.writes == 0, "stale snapshot wrote storage");
  stale.sentinel();
}

class SnapshotTransport final : public vikey::IVikeyCardJournalTransport {
 public:
  explicit SnapshotTransport(const Memory& source) : media(source) {}
  SecureBuffer snapshot_reserved() override {
    require_lease();
    const auto call = snapshots++;
    if (media.offline) throw std::system_error(make_error_code(CardError::communication_failed));
    if (call == change_on_snapshot) media.data[1807] ^= std::byte{1};
    return copy(std::span(media.data).first(call == short_snapshot ? 1807 : 1808));
  }
  void write_block(const SecureBuffer& expected, std::size_t offset, const SecureBuffer& block) override {
    require_lease();
    if (change_before_write) media.data[1807] ^= std::byte{1};
    if (!same(expected.bytes(), std::span(media.data).first(1808))) {
      throw std::system_error(make_error_code(CardError::invalid_operation));
    }
    const auto call = media.writes;
    media.write(offset, block.bytes());
    if (corrupt_after_write) media.data[0] ^= std::byte{1};
    if (call == reported_error) throw std::system_error(make_error_code(CardError::communication_failed));
  }
  Memory media;
  std::size_t snapshots{};
  std::size_t short_snapshot{std::numeric_limits<std::size_t>::max()};
  std::size_t change_on_snapshot{std::numeric_limits<std::size_t>::max()};
  std::size_t reported_error{std::numeric_limits<std::size_t>::max()};
  bool change_before_write{};
  bool corrupt_after_write{};
 public:
  void require_lease() {
    bool blocked = false;
    std::thread competitor([&] {
      try { vikey::CardOperationLease lease; }
      catch (const std::system_error& error) { blocked = error.code().value() == ERROR_BUSY; }
    });
    competitor.join();
    expect(blocked, "whole transaction lease missing during transport call");
  }
};

void test_application_journal_adapter(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  SnapshotTransport transport(base);
  vikey::VikeyCardJournalStore store(transport);
  expect(same(store.snapshot().bytes(), initial.record.bytes()), "application snapshot selected wrong record");
  expect(transport.media.writes == 0, "application snapshot normalized journal");
  store.recover(initial.record.bytes(), initial.root_secret);
  store.commit(initial.record.bytes(), replacement.bytes(), initial.root_secret);
  expect(same(store.snapshot().bytes(), replacement.bytes()) && transport.media.writes == 4,
         "application conditional append failed");
  store.commit(initial.record.bytes(), replacement.bytes(), initial.root_secret);
  expect(transport.media.writes == 4, "completed candidate was appended twice");
  rejects<std::system_error>([&] {
    store.recover(initial.record.bytes(), initial.root_secret);
  }, "application stale logical snapshot permitted recovery");
  expect(transport.media.writes == 4, "stale application recovery wrote card");
  transport.media.sentinel();
  SecureBuffer wrong_root(32);
  SnapshotTransport rejected(base);
  vikey::VikeyCardJournalStore rejected_store(rejected);
  rejects<CardCredentialRejected>([&] {
    rejected_store.recover(initial.record.bytes(), wrong_root);
  }, "application repair accepted wrong root");
  rejects<CardCredentialRejected>([&] {
    rejected_store.commit(initial.record.bytes(), replacement.bytes(), wrong_root);
  }, "application append accepted wrong root");
  expect(rejected.media.writes == 0, "application root rejection wrote card");
  SnapshotTransport bad_anchor(base);
  bad_anchor.media.data[304 + 184] ^= std::byte{1};
  for (const auto offset : {0U, 152U}) {
    auto digest = sha256(std::span(bad_anchor.media.data).subspan(304, 216));
    std::ranges::copy(digest, bad_anchor.media.data.begin() + offset + 56);
    rehash_descriptor(bad_anchor.media, offset);
  }
  vikey::VikeyCardJournalStore bad_anchor_store(bad_anchor);
  rejects<CardCredentialRejected>([&] {
    bad_anchor_store.recover(initial.record.bytes(), initial.root_secret);
  }, "application recovery relied on public checksums instead of anchor MAC");
  expect(bad_anchor.media.writes == 0, "bad application anchor permitted cleanup writes");
  for (unsigned int mode = 0; mode < 5; ++mode) {
    SnapshotTransport invalid(base);
    if (mode == 0) invalid.short_snapshot = 0;
    if (mode == 1) invalid.change_before_write = true;
    if (mode == 2) invalid.corrupt_after_write = true;
    if (mode == 3) invalid.short_snapshot = 1;
    if (mode == 4) invalid.change_on_snapshot = 5; // Final read after four writes.
    vikey::VikeyCardJournalStore invalid_store(invalid);
    rejects<std::system_error>([&] {
      invalid_store.commit(initial.record.bytes(), replacement.bytes(), initial.root_secret);
    }, "unverified complete postimage reported success");
    expect(invalid.media.writes == (mode == 0 || mode == 1 ? 0 : mode == 4 ? 4 : 1),
           "failed transaction performed additional writes");
    invalid.media.sentinel();
  }
}

void test_application_transport_faults(const InitializedCardRecordV2& initial,
    const Memory& base, const SecureBuffer& replacement) {
  for (std::size_t call = 0; call < 4; ++call) {
    SnapshotTransport full(base);
    full.reported_error = call;
    vikey::VikeyCardJournalStore full_store(full);
    full_store.commit(initial.record.bytes(), replacement.bytes(), initial.root_secret);
    expect(full.media.writes == 4 && same(full_store.snapshot().bytes(), replacement.bytes()),
           "verified complete write error failed or advanced twice");
    for (const std::size_t mode : {0U, 2U, 1U}) {
      SnapshotTransport interrupted(base);
      interrupted.media.cut_write = call;
      interrupted.media.tear_mode = mode;
      vikey::VikeyCardJournalStore interrupted_store(interrupted);
      rejects<std::system_error>([&] {
        interrupted_store.commit(initial.record.bytes(), replacement.bytes(), initial.root_secret);
      }, "offline readback allowed transaction continuation");
      expect(interrupted.media.writes == call + 1, "offline write was blindly retried");
      interrupted.media.online();
      auto selected = interrupted_store.snapshot();
      expect(same(selected.bytes(), initial.record.bytes()) || same(selected.bytes(), replacement.bytes()),
             "interrupted application write selected an unrelated record");
      interrupted_store.recover(selected.bytes(), initial.root_secret);
      interrupted.media.sentinel();
    }
  }
}

void test_full_reserved_reset(const InitializedCardRecordV2& initial, const Memory& base) {
  Memory source = base;
  const auto expected = copy(std::span(source.data).first(1808));
  auto plan = prepare_card_journal_v2_reset(source, expected.bytes());
  expect(source.writes == 0, "reset preparation wrote card");
  Memory completed = source;
  reset_card_journal_v2(completed, plan);
  expect(completed.writes == 10, "reset did not revoke, replace, clear, and publish every block");
  const auto target = copy(std::span(completed.data).first(1808));
  auto marker = snapshot_card_record_journal_v1(completed);
  expect(crypto::is_card_record_v2_provisioning_marker(marker.bytes()) &&
      inspect_card_record(marker.bytes()) == CardRecordState::unsupported_version,
      "reset exposed a legacy-compatible marker");
  expect(std::ranges::all_of(std::span(completed.data).subspan(736, 1072),
      [](std::byte b) { return b == std::byte{}; }), "reset left extension credential copies");
  completed.online();
  reset_card_journal_v2(completed, plan);
  expect(completed.writes == 0, "completed reset wrote again");
  rejects<std::system_error>([&] { reset_card_record_journal_v1(completed, initial.record.bytes()); },
      "legacy reset accepted the v2 reset marker");
  auto old_marker = provision_uninitialized_card_record_v1();
  rejects<std::system_error>([&] { bootstrap_uninitialized_card_record_journal_v1(completed, old_marker.bytes()); },
      "legacy bootstrap accepted the v2 reset marker");
  rejects<std::system_error>([&] { commit_card_record_journal_v1(completed, old_marker.bytes()); },
      "legacy writer accepted the v2 reset marker");
  expect(completed.writes == 0, "legacy reset-marker rejection wrote card");
  completed.sentinel();

  for (std::size_t call = 0; call < 10; ++call) {
    for (const std::size_t mode : {0U, 2U, 1U}) {
      Memory interrupted = source;
      interrupted.cut_write = call;
      interrupted.tear_mode = mode;
      rejects<std::system_error>([&] { reset_card_journal_v2(interrupted, plan); },
          "reset interruption reported success");
      expect(interrupted.writes == call + 1, "reset blindly retried uncertain write");
      interrupted.online();
      Memory reopened = interrupted;
      auto freshly_confirmed = prepare_card_journal_v2_reset(reopened, std::span(reopened.data).first(1808));
      reset_card_journal_v2(reopened, freshly_confirmed);
      expect(crypto::is_card_record_v2_provisioning_marker(snapshot_card_record_journal_v1(reopened).bytes()),
          "freshly confirmed reset could not discard interrupted residue");
      reopened.sentinel();
      reset_card_journal_v2(interrupted, plan);
      expect(same(std::span(interrupted.data).first(1808), target.bytes()), "reset retry changed target");
      interrupted.sentinel();
    }
    Memory reported = source;
    reported.report_error_after_write = call;
    reset_card_journal_v2(reported, plan);
    expect(same(std::span(reported.data).first(1808), target.bytes()), "complete reset write error lost target");
  }
  Memory changed = source;
  changed.data[1807] ^= std::byte{1};
  rejects<std::system_error>([&] { reset_card_journal_v2(changed, plan); }, "stale reset snapshot accepted");
  expect(changed.writes == 0, "stale reset wrote card");
  rejects<std::system_error>([&] {
    static_cast<void>(prepare_card_journal_v2_reset(changed, expected.bytes()));
  }, "stale reset preparation accepted a different full snapshot");
  Memory small = source;
  small.reported_capacity = 1807;
  rejects<std::system_error>([&] { static_cast<void>(prepare_card_journal_v2_reset(small, expected.bytes())); },
      "reset accepted insufficient capacity");
  Memory short_read = source;
  short_read.short_read = 0;
  rejects<std::system_error>([&] { static_cast<void>(prepare_card_journal_v2_reset(short_read, expected.bytes())); },
      "reset accepted short full snapshot");
  expect(small.writes == 0 && short_read.writes == 0, "reset shape rejection wrote card");
  Memory legacy;
  auto legacy_marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(legacy, legacy_marker.bytes());
  SecureBuffer legacy_record(216);
  std::ranges::copy(initial.record.bytes().subspan(32, 64), legacy_record.bytes().begin());
  commit_card_record_journal_v1(legacy, legacy_record.bytes());
  const auto inactive = crypto::card_journal_u32(std::span(legacy.data), 48) == 0 ? 520U : 304U;
  std::ranges::copy(legacy_marker.bytes(), legacy.data.begin() + inactive);
  const auto inactive_hash = sha256(legacy_marker.bytes());
  for (const auto offset : {0U, 152U}) {
    std::ranges::copy(inactive_hash, legacy.data.begin() + offset + 88);
    rehash_descriptor(legacy, offset);
  }
  legacy.online();
  rejects<std::system_error>([&] {
    static_cast<void>(prepare_card_journal_v2_reset(legacy, std::span(legacy.data).first(1808)));
  }, "v2 reset accepted an active old initialized journal");
  expect(legacy.writes == 0, "active legacy reset rejection wrote card");
  for (const auto offset : {736U, 1040U}) {
    Memory future = source;
    crypto::card_journal_store_u32(std::span(future.data), offset + 8, 9);
    rejects<std::system_error>([&] {
      static_cast<void>(prepare_card_journal_v2_reset(future, std::span(future.data).first(1808)));
    }, "reset accepted future extension format");
    expect(future.writes == 0, "future reset rejection wrote card");
  }
  auto new_plan = prepare_card_journal_v2_initialization(completed, marker.bytes(), text(kPasswords[2]));
  auto new_root = commit_card_journal_v2_initialization(completed, new_plan);
  expect(!same(new_root.bytes(), initial.root_secret.bytes()), "reset initialization reused old root");
  const auto record = snapshot_card_record_journal_v2(completed);
  expect(same(unlock_card_record_v2(record.bytes(), text(kPasswords[2])).bytes(), new_root.bytes()),
      "reset initialization credential failed");
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(record.bytes(), text(kPasswords[0])));
  }, "old password survived reset initialization");
  completed.online();
  rejects<std::system_error>([&] { reset_card_journal_v2(completed, plan); },
      "old reset plan erased the freshly initialized card");
  expect(completed.writes == 0, "expired reset plan wrote the new card");
  completed.sentinel();
  std::cout << "reset faults: 30; complete-write error checks: 10; fresh-root check passed\n";
}

void test_application_reset_adapter(const Memory& base) {
  SnapshotTransport corrupt(base);
  vikey::VikeyCardJournalStore corrupt_store(corrupt);
  auto corrupt_plan = corrupt_store.prepare_reset(corrupt_store.snapshot_for_reset().bytes());
  corrupt.corrupt_after_write = true;
  rejects<std::system_error>([&] { corrupt_store.reset(corrupt_plan); },
      "reset accepted corruption outside its complete postimage");
  expect(corrupt.media.writes == 1, "corrupted reset continued writing");
  for (std::size_t call = 0; call < 10; ++call) {
    SnapshotTransport transport(base), other(base);
    vikey::VikeyCardJournalStore store(transport), other_store(other);
    auto snapshot = store.snapshot_for_reset();
    auto plan = store.prepare_reset(snapshot.bytes());
    expect(transport.media.writes == 0, "application reset preparation wrote card");
    rejects<std::system_error>([&] { other_store.reset(plan); }, "reset accepted a different store instance");
    expect(other.media.writes == 0, "cross-store reset wrote card");
    transport.media.cut_write = call;
    transport.media.tear_mode = 2;
    rejects<std::system_error>([&] { store.reset(plan); }, "application reset accepted incomplete postimage");
    expect(transport.media.writes == call + 1, "application reset retried uncertain write");
    transport.media.online();
    store.reset(plan);
    auto marker = store.snapshot_provisioning();
    expect(crypto::is_card_record_v2_provisioning_marker(marker.bytes()), "application reset failed marker publication");
    const auto writes = transport.media.writes;
    store.reset(plan);
    expect(transport.media.writes == writes, "application reset was repeated after completion");
    transport.media.sentinel();
  }
}

void test_read_only_product_entry(const Memory& base) {
  Memory legacy_media;
  auto old_marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(legacy_media, old_marker.bytes());
  SnapshotTransport legacy(legacy_media);
  vikey::VikeyCardJournalStore legacy_store(legacy);
  expect(legacy_store.inspect_entry() == CardProductRecordState::provisioned_uninitialized && legacy.media.writes == 0,
      "old provisioning entry changed during v2 classification");
  std::ranges::copy(crypto::kCardV2AnchorMagic, legacy.media.data.begin() + 1040);
  crypto::card_journal_store_u32(std::span(legacy.media.data), 1040 + 8, 9);
  expect(legacy_store.inspect_entry() == CardProductRecordState::unsupported_version && legacy.media.writes == 0,
      "old marker bypassed future staging rejection");
  std::ranges::fill(std::span(legacy.media.data).subspan(736, 1072), std::byte{});
  SecureBuffer old_record(216);
  std::ranges::copy(std::span(base.data).subspan(1040 + 32, 64), old_record.bytes().begin());
  commit_card_record_journal_v1(legacy.media, old_record.bytes());
  legacy.media.online();
  expect(legacy_store.inspect_entry() == CardProductRecordState::initialized_v1 && legacy.media.writes == 0,
      "old initialized entry was misclassified as v2");
  SnapshotTransport transport(base);
  vikey::VikeyCardJournalStore store(transport);
  expect(store.inspect_entry() == CardProductRecordState::initialized_v2,
      "supported entry did not recognize v2 journal");
  transport.media.data[888] ^= std::byte{1};
  const auto before = transport.media.data;
  expect(store.inspect_entry() == CardProductRecordState::initialized_v2 &&
      transport.media.writes == 0 && transport.media.data == before, "entry inspection normalized journal");
  transport.media.data[1040 + 140] ^= std::byte{1};
  rejects<std::system_error>([&] { static_cast<void>(store.inspect_entry()); },
      "damaged selected v2 record was admitted");
  expect(transport.media.writes == 0, "damaged entry inspection wrote card");

  SnapshotTransport reset(base);
  vikey::VikeyCardJournalStore reset_store(reset);
  auto reset_plan = reset_store.prepare_reset(reset_store.snapshot_for_reset().bytes());
  reset_store.reset(reset_plan);
  reset.media.online();
  expect(reset_store.inspect_entry() == CardProductRecordState::provisioned_uninitialized,
      "strict v2 marker did not reach initialization contract");
  auto marker = reset_store.snapshot_provisioning();
  expect(inspect_card_record(marker.bytes()) == CardRecordState::unsupported_version,
      "new entry evidence weakened legacy version rejection");
  const auto saved = reset.media.data;
  std::ranges::copy(crypto::kCardV2AnchorMagic, reset.media.data.begin() + 1040);
  crypto::card_journal_store_u32(std::span(reset.media.data), 1040 + 8, 9);
  expect(reset_store.inspect_entry() == CardProductRecordState::unsupported_version && reset.media.writes == 0,
      "future staged record reached initialization route");
  reset.media.data = saved;
  reset.media.data[304 + 32] = std::byte{1};
  const auto digest = sha256(std::span(reset.media.data).subspan(304, 216));
  for (const auto offset : {0U, 152U}) {
    std::ranges::copy(digest, reset.media.data.begin() + offset + 56);
    rehash_descriptor(reset.media, offset);
  }
  expect(reset_store.inspect_entry() == CardProductRecordState::unsupported_version && reset.media.writes == 0,
      "noncanonical future marker reached initialization");
  for (const bool short_read : {true, false}) {
    SnapshotTransport unstable(base);
    if (short_read) unstable.short_snapshot = 0;
    else unstable.change_on_snapshot = 1;
    vikey::VikeyCardJournalStore unstable_store(unstable);
    rejects<std::system_error>([&] { static_cast<void>(unstable_store.inspect_entry()); },
        "entry admitted short or changed full snapshot");
    expect(unstable.media.writes == 0, "unstable entry inspection wrote card");
  }
  transport.media.sentinel();
  reset.media.sentinel();
}

void test_v2_login_target(const InitializedCardRecordV2& initial, const Memory& base) {
  SnapshotTransport transport(base);
  vikey::VikeyCardJournalLoginTarget target(transport);
  expect(transport.snapshots == 0 && transport.media.writes == 0, "v2 login target construction performed I/O");
  expect(target.record_format() == CardProductRecordState::initialized_v2 &&
      same(target.read_record().bytes(), initial.record.bytes()) && transport.media.writes == 0,
      "v2 login read changed storage or lost format identity");
  SecureBuffer wrong(32);
  rejects<CardCredentialRejected>([&] { target.complete_authenticated_read(initial.record.bytes(), wrong); },
      "v2 login target accepted wrong root");
  expect(transport.media.writes == 0, "wrong v2 login root wrote card");
  target.complete_authenticated_read(initial.record.bytes(), initial.root_secret);
  expect(transport.media.writes == 0, "healthy v2 login normalized storage");

  SnapshotTransport pending(base);
  pending.media.data[1424 + 383] ^= std::byte{1};
  vikey::VikeyCardJournalLoginTarget repairing(pending);
  const auto before = pending.media.data;
  expect(same(repairing.read_record().bytes(), initial.record.bytes()) && pending.media.writes == 0 &&
      pending.media.data == before, "unauthenticated v2 login repaired residue");
  repairing.complete_authenticated_read(initial.record.bytes(), initial.root_secret);
  expect(pending.media.writes == 3, "authenticated v2 login failed to normalize inactive journal");
  pending.media.sentinel();

  SnapshotTransport forged(base);
  forged.media.data[304 + 184] ^= std::byte{1};
  const auto digest = sha256(std::span(forged.media.data).subspan(304, 216));
  for (const auto offset : {0U, 152U}) {
    std::ranges::copy(digest, forged.media.data.begin() + offset + 56);
    rehash_descriptor(forged.media, offset);
  }
  vikey::VikeyCardJournalLoginTarget invalid(forged);
  expect(same(invalid.read_record().bytes(), initial.record.bytes()), "public snapshot incorrectly claimed anchor authentication");
  rejects<CardCredentialRejected>([&] { invalid.complete_authenticated_read(initial.record.bytes(), initial.root_secret); },
      "v2 login relied on public hashes instead of anchor MAC");
  expect(forged.media.writes == 0, "forged v2 login anchor wrote card");
  forged.media.sentinel();
}

class NoCredentials final : public vikey::IVikeyCredentialProvider {
 public:
  vikey::VikeyCredentialSet load_credentials() override {
    throw std::runtime_error("injected ciphertext operations loaded vendor credentials");
  }
};

class EnrollmentIo final : public vikey::IVikeyCardEnrollmentIo {
 public:
  explicit EnrollmentIo(const Memory& base) : media(base) {}
  vikey::VikeyCardMediaProbe observed{0x55AA, 1,
      vikey::VikeyCardMediaState::product_evidence, CardRecordState::unsupported_version};
  SnapshotTransport media;
  std::uint32_t actual_id{1};
  int bootstraps{};
  bool fail_probe{};
  int initialization_fault{};
  class Bound final : public vikey::IVikeyCardJournalTransport {
   public:
    Bound(EnrollmentIo& io, std::uint32_t id) : io_(io), id_(id) {}
    SecureBuffer snapshot_reserved() override { check(); return io_.media.snapshot_reserved(); }
    void write_block(const SecureBuffer& expected, std::size_t offset, const SecureBuffer& replacement) override {
      check(); io_.media.write_block(expected, offset, replacement);
    }
   private:
    void check() {
      if (id_ != io_.actual_id) throw std::system_error(make_error_code(CardError::invalid_operation));
    }
    EnrollmentIo& io_;
    std::uint32_t id_;
  };
  vikey::VikeyCardMediaProbe probe() override {
    media.require_lease();
    if (fail_probe) throw std::system_error(make_error_code(CardError::communication_failed));
    return observed;
  }
  std::unique_ptr<vikey::IVikeyCardJournalTransport> transport(std::uint32_t id) override {
    expect(id == observed.hardware_id, "source lost probed hardware identity");
    return std::make_unique<Bound>(*this, id);
  }
  void bootstrap(const vikey::VikeyCardMediaProbe& context) override {
    media.require_lease();
    expect(context.hardware_id == actual_id, "bootstrap used another card");
    ++bootstraps;
    auto marker = provision_uninitialized_card_record_v1();
    provision_card_record_journal_v1(media.media, marker.bytes());
    if (initialization_fault == 1) media.media.cut_write = media.media.writes;
    if (initialization_fault == 2) media.reported_error = media.media.writes;
  }
};

class LoginConsumer final : public usb_vault::application::ICardLoginSuccessConsumer {
 public:
  SecureBuffer root;
  void consume_authenticated_login(SecureBuffer value, SecureBuffer password) override {
    expect(same(password.bytes(), text(kPasswords[0]).bytes()), "source login changed exact input");
    root = std::move(value);
  }
};

security::ConfirmedPassword confirmed_initial_password(std::string_view password = kPasswords[0]) {
  class Policy final : public security::IPasswordStrengthPolicy {
   public:
    security::PasswordStrength assess(const SecureBuffer&) const noexcept override {
      return security::PasswordStrength::acceptable;
    }
  } policy;
  return security::confirm_password(policy, text(password), text(password));
}

void test_enrollment_login_composition(const InitializedCardRecordV2& initial, const Memory& base) {
  namespace app = usb_vault::application;
  NoCredentials credentials;
  EnrollmentIo io(base);
  vikey::VikeyHelperCardEnrollment source({}, credentials, &io), other({}, credentials, &io);
  auto context = source.inspect_entry();
  expect(app::CardEntryRouter::route(*context) == app::CardEntryRoute::login_required &&
      io.media.media.writes == 0, "source did not classify v2 read-only");
  rejects<std::system_error>([&] { static_cast<void>(other.make_login_target(*context)); },
      "source accepted another source's context");
  auto current = source.inspect_entry();
  rejects<std::system_error>([&] { static_cast<void>(source.make_login_target(*context)); },
      "source accepted expired context");
  const auto reads = io.media.snapshots;
  auto target = source.make_login_target(*current);
  expect(io.media.snapshots == reads, "login factory performed I/O");
  expect(target->record_format() == CardProductRecordState::initialized_v2,
      "source created legacy login target for v2");
  io.fail_probe = true;
  rejects<std::system_error>([&] { static_cast<void>(source.inspect_entry()); }, "failed replacement probe succeeded");
  rejects<std::system_error>([&] { static_cast<void>(source.make_login_target(*current)); },
      "failed replacement probe retained context");
  io.fail_probe = false;
  app::CardEntryContextController controller(source);
  LoginConsumer consumer;
  expect(controller.handle_arrival() == app::CardEntryRoute::login_required, "v2 arrival did not reach login");
  rejects<CardCredentialRejected>([&] { controller.authenticate_login(consumer, text("wrong-password")); },
      "wrong v2 password authenticated through source");
  expect(controller.has_context() && consumer.root.empty() && io.media.media.writes == 0,
      "wrong password lost retry or wrote card");
  controller.authenticate_login(consumer, text(kPasswords[0]));
  expect(same(consumer.root.bytes(), initial.root_secret.bytes()) && !controller.has_context(),
      "source login failed root handoff or context retirement");
  consumer.root = SecureBuffer{};
  static_cast<void>(controller.handle_arrival());
  io.actual_id = 2;
  rejects<std::system_error>([&] { controller.authenticate_login(consumer, text(kPasswords[0])); },
      "source login accepted swapped hardware");
  expect(consumer.root.empty() && !controller.has_context() && io.media.media.writes == 0,
      "swapped card established session or wrote card");
  io.media.media.sentinel();
}

void test_enrollment_initialization_composition(const Memory& base) {
  namespace app = usb_vault::application;
  NoCredentials credentials;
  for (int kind = 0; kind < 5; ++kind) {
    EnrollmentIo io(base);
    if (kind == 0 || kind >= 3) {
      io.observed.media_state = vikey::VikeyCardMediaState::blank;
      io.observed.logical_state.reset();
      std::ranges::fill_n(io.media.media.data.begin(), 1808, std::byte{});
    } else if (kind == 1) {
      auto marker = provision_uninitialized_card_record_v1();
      std::ranges::fill_n(io.media.media.data.begin(), 1808, std::byte{});
      provision_card_record_journal_v1(io.media.media, marker.bytes());
      io.observed.logical_state = CardRecordState::provisioned_uninitialized;
    } else {
      auto plan = prepare_card_journal_v2_reset(io.media.media, std::span(io.media.media.data).first(1808));
      reset_card_journal_v2(io.media.media, plan);
    }
    io.media.media.online();
    vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
    auto insertion = source.inspect_entry();
    const auto reads_before_factory = io.media.snapshots;
    auto target = source.make_initialization_target(*insertion);
    expect(io.media.snapshots == reads_before_factory && io.media.media.writes == 0 && io.bootstraps == 0,
        "initialization factory performed I/O");
    rejects<std::logic_error>([&] { static_cast<void>(target->card_store()); },
        "journal initialization exposed arbitrary legacy writes");
    target.reset();
    app::CardEntryContextController controller(source);
    app::CardAuthenticationSession session;
    expect(controller.handle_arrival() == app::CardEntryRoute::initialization_required &&
        io.media.media.writes == 0 && io.bootstraps == 0, "initialization arrival wrote or lost marker");
    if (kind == 3) io.initialization_fault = 1;
    if (kind == 4) io.initialization_fault = 2;
    try {
      controller.initialize_and_authenticate(session, confirmed_initial_password());
      expect(kind != 3, "uncertain initialization reported success");
    } catch (const std::system_error&) {
      expect(kind == 3, "healthy initialization failed");
    }
    expect(!controller.has_context() && session.authenticated() == (kind != 3),
        "initialization failure retained context or reported authentication");
    if (kind != 3) {
      vikey::VikeyCardJournalStore store(io.media);
      auto record = store.snapshot();
      auto root = unlock_card_record_v2(record.bytes(), text(kPasswords[0]));
      store.recover(record.bytes(), root);
      expect(inspect_card_record_v2(record.bytes()) == CardRecordV2State::initialized &&
          io.bootstraps == (kind == 0 || kind == 4 ? 1 : 0), "initialization did not publish v2 or repeated bootstrap");
      io.observed.media_state = vikey::VikeyCardMediaState::product_evidence;
      io.observed.logical_state = CardRecordState::unsupported_version;
      expect(source.inspect_entry()->inspect_supported_product_record_read_only() ==
          CardProductRecordState::initialized_v2, "initialized card did not reopen as v2");
    } else {
      io.media.media.online();
      io.observed = {0x55AA, 1, vikey::VikeyCardMediaState::product_evidence,
          CardRecordState::provisioned_uninitialized};
      app::CardEntryContextController cleanup(source, {}, [] { return true; });
      static_cast<void>(cleanup.handle_arrival());
      expect(cleanup.can_login_reset() && io.media.media.writes == 0,
          "failed initialization prevented explicit destruction or repaired during inspection");
      cleanup.confirm_login_reset(cleanup.prepare_login_reset());
    }
    io.media.media.sentinel();
  }
}

void test_product_password_ring(const InitializedCardRecordV2& initial, const Memory& base) {
  namespace app = usb_vault::application;
  NoCredentials credentials;
  EnrollmentIo io(base);
  vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
  int confirmations = 0;
  app::CardEntryContextController controller(source, [&] { ++confirmations; return true; });
  for (std::size_t added = 1; added < kPasswords.size(); ++added) {
    controller.set_second_card_password(text(kPasswords[added < 4 ? 0 : 1]), text(kPasswords[added]));
    expect(confirmations == static_cast<int>(added) && !controller.has_context(),
        "append did not consume exactly one confirmation and context");
    vikey::VikeyCardJournalStore store(io.media);
    auto record = store.snapshot();
    for (std::size_t password = 0; password <= added; ++password) {
      if (password + 3 <= added) {
        rejects<CardCredentialRejected>([&] { static_cast<void>(unlock_card_record_v2(record.bytes(), text(kPasswords[password]))); },
            "product append retained an evicted credential");
      } else {
        auto root = unlock_card_record_v2(record.bytes(), text(kPasswords[password]));
        expect(same(root.bytes(), initial.root_secret.bytes()), "product append changed the card root");
      }
    }
    io.media.media.sentinel();
  }
}

void test_product_update_rejections(const Memory& base) {
  namespace app = usb_vault::application;
  NoCredentials credentials;
  for (int scenario = 0; scenario < 7; ++scenario) {
    EnrollmentIo io(base);
    vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
    int confirmations = 0;
    const auto confirm = [&] {
      ++confirmations;
      if (scenario == 3) io.actual_id = 2;
      if (scenario == 4) io.media.media.data[1040 + 383] ^= std::byte{1};
      if (scenario == 5) io.media.media.cut_write = 0;
      return scenario != 0;
    };
    app::CardEntryContextController controller(source,
        scenario == 6 ? std::function<bool()>{} : std::function<bool()>(confirm));
    bool rejected = false;
    try {
      controller.set_second_card_password(text(scenario == 2 ? "wrong-password" : kPasswords[0]),
          text(scenario == 1 ? kPasswords[0] : kPasswords[1]));
    } catch (const std::exception&) { rejected = true; }
    expect(rejected && !controller.has_context() && io.media.media.writes == (scenario == 5 ? 1U : 0U),
        "cancelled, invalid, stale, swapped, or torn update continued writing");
    expect(confirmations == (scenario == 1 || scenario == 2 || scenario == 6 ? 0 : 1),
        "invalid password or missing callback reached confirmation");
    io.media.media.sentinel();
  }
  EnrollmentIo repeated(base);
  vikey::VikeyHelperCardEnrollment source({}, credentials, &repeated);
  auto context = source.inspect_entry();
  auto target = source.make_password_update_target(*context);
  rejects<std::system_error>([&] { target->append_password(text(kPasswords[0]), text(kPasswords[1]), [] { return false; }); },
      "cancelled direct target succeeded");
  rejects<std::system_error>([&] { target->append_password(text(kPasswords[0]), text(kPasswords[1]), [] { return true; }); },
      "consumed direct target was reused");
  expect(repeated.media.media.writes == 0, "replayed cancelled target wrote card");
}

void test_product_legacy_upgrade_and_append() {
  namespace app = usb_vault::application;
  NoCredentials credentials;
  for (const bool two_passwords : {false, true}) {
    auto initial = initialize_card_record_and_root_v1(text(kPasswords[0]));
    auto legacy = two_passwords
        ? rewrap_card_record_v1(initial.record.bytes(), text(kPasswords[0]), text(kPasswords[1]), text(kPasswords[0]))
        : copy(initial.record.bytes());
    Memory storage;
    auto marker = provision_uninitialized_card_record_v1();
    provision_card_record_journal_v1(storage, marker.bytes());
    commit_card_record_journal_v1(storage, legacy.bytes());
    storage.online();
    EnrollmentIo io(storage);
    io.observed.logical_state = CardRecordState::initialized_v1;
    vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
    app::CardEntryContextController cancelled(source, [] { return false; });
    const auto before = io.media.media.data;
    rejects<std::system_error>([&] { cancelled.set_second_card_password(text(kPasswords[0]), text(kPasswords[2])); },
        "cancelled upgrade succeeded");
    expect(io.media.media.data == before && io.media.media.writes == 0,
        "upgrade preparation or cancellation changed old credentials");
    int confirmations = 0;
    app::CardEntryContextController controller(source, [&] { ++confirmations; return true; });
    controller.set_second_card_password(text(kPasswords[0]), text(kPasswords[2]));
    expect(confirmations == 1 && !controller.has_context(), "upgrade and append used multiple confirmations");
    vikey::VikeyCardJournalStore store(io.media);
    auto record = store.snapshot();
    expect(same(record.bytes().subspan(32, 64), legacy.bytes().first(64)) &&
        same(record.bytes().subspan(96, 152), legacy.bytes().subspan(64)),
        "product upgrade replaced retained legacy context or envelopes");
    for (const auto password : {kPasswords[0], kPasswords[2]}) {
      expect(same(unlock_card_record_v2(record.bytes(), text(password)).bytes(), initial.root_secret.bytes()),
          "upgrade and append changed root or lost password");
    }
    if (two_passwords) expect(same(unlock_card_record_v2(record.bytes(), text(kPasswords[1])).bytes(), initial.root_secret.bytes()),
        "product upgrade lost the second legacy password");
    io.media.media.sentinel();
  }
}

void test_product_reset_composition(const InitializedCardRecordV2& initial, const Memory& base) {
  namespace app = usb_vault::application;
  for (int scenario = 0; scenario < 12; ++scenario) {
    NoCredentials credentials;
    EnrollmentIo io(base);
    vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
    int confirmations = 0;
    app::CardEntryContextController controller(source, {}, [&] {
      ++confirmations;
      if (scenario == 10) io.actual_id = 2;
      return scenario != 8;
    });
    if (scenario == 11) io.media.media.data[1040 + 383] ^= std::byte{1};
    expect(controller.handle_arrival() == (scenario == 11 ? app::CardEntryRoute::unavailable : app::CardEntryRoute::login_required),
        "reset source lost v2 login route");
    const auto ticket = controller.prepare_login_reset();
    expect(io.media.media.writes == 0 && confirmations == 0 && controller.login_reset_pending(ticket),
        "reset preparation wrote card or confirmed destruction");
    if (scenario == 1) controller.cancel_login_reset();
    if (scenario == 2) controller.handle_removal();
    if (scenario == 3) static_cast<void>(controller.handle_arrival());
    if (scenario == 4) io.actual_id = 2;
    if (scenario == 5) io.media.media.data[1424 + 383] ^= std::byte{1};
    if (scenario == 6) { io.media.media.cut_write = 1; io.media.media.tear_mode = 2; }
    if (scenario == 7) io.media.reported_error = 9;
    if (scenario == 9) {
      crypto::card_journal_store_u32(std::span(io.media.media.data), 1040 + 8, 9);
      static_cast<void>(controller.handle_arrival());
      expect(!controller.can_login_reset(), "future record offered destructive reset");
    }
    bool success = false;
    try { controller.confirm_login_reset(ticket); success = true; } catch (const std::exception&) {}
    if (success != (scenario == 0 || scenario == 7 || scenario == 11))
      std::cerr << "reset scenario " << scenario << "; success " << success << '\n';
    expect(success == (scenario == 0 || scenario == 7 || scenario == 11),
        "product reset accepted stale confirmation or misreported verified write");
    expect(!controller.login_reset_pending(ticket), "reset retained a consumed preparation");
    const auto writes = io.media.media.writes;
    rejects<std::system_error>([&] { controller.confirm_login_reset(ticket); }, "reset ticket replay succeeded");
    expect(io.media.media.writes == writes, "reset ticket replay wrote card");
    if (scenario != 0 && scenario != 6 && scenario != 7 && scenario != 11)
      expect(writes == 0, "rejected product reset wrote card");
    if (scenario == 6) {
      io.media.media.online();
      io.observed.logical_state = CardRecordState::malformed;
      expect(controller.handle_arrival() == app::CardEntryRoute::unavailable && controller.can_login_reset(),
          "lost reset plan promoted residue or prevented newly confirmed cleanup");
      const auto fresh = controller.prepare_login_reset();
      expect(io.media.media.writes == 0, "residue inspection repaired before confirmation");
      controller.confirm_login_reset(fresh);
      expect(confirmations == 2, "residue cleanup reused destructive authorization");
    }
    if (scenario == 0 || scenario == 6 || scenario == 7 || scenario == 11) {
      io.observed = {0x55AA, 1, vikey::VikeyCardMediaState::product_evidence, CardRecordState::unsupported_version};
      expect(controller.handle_arrival() == app::CardEntryRoute::initialization_required,
          "complete reset failed strict marker admission");
      if (scenario == 0) {
        app::CardAuthenticationSession authentication;
        controller.initialize_and_authenticate(authentication, confirmed_initial_password("initial-password-strong"));
        vikey::VikeyCardJournalStore reopened(io.media);
        auto record = reopened.snapshot();
        auto root = unlock_card_record_v2(record.bytes(), text("initial-password-strong"));
        expect(!same(root.bytes(), initial.root_secret.bytes()), "reinitialization retained destroyed root");
        rejects<CardCredentialRejected>([&] { static_cast<void>(unlock_card_record_v2(record.bytes(), text(kPasswords[0]))); },
            "reset retained an old password");
      }
    }
    io.media.media.sentinel();
  }
  NoCredentials credentials;
  EnrollmentIo io(base);
  vikey::VikeyHelperCardEnrollment source({}, credentials, &io);
  app::CardEntryContextController no_confirmation(source);
  static_cast<void>(no_confirmation.handle_arrival());
  const auto ticket = no_confirmation.prepare_login_reset();
  rejects<std::system_error>([&] { no_confirmation.confirm_login_reset(ticket); }, "missing native reset confirmation accepted");
  expect(io.media.media.writes == 0, "missing native confirmation wrote card");
  app::CardAuthenticationSession authentication;
  authentication.establish(copy(initial.root_secret.bytes()));
  app::CardEntryContextController authenticated(source, {}, [] { return true; });
  authenticated.reset_card_record(authentication);
  expect(!authenticated.has_context() && io.media.media.writes == 10,
      "authenticated reset did not use full reserved-region transaction");
  io.media.media.sentinel();
}

} // namespace

int main() {
  try {
    auto initial = initialize_card_record_and_root_v2(text(kPasswords[0]));
    Memory base;
    fixture(base, initial);
    test_product_reset_composition(initial, base);
    auto replacement = append_card_password_v2(initial.record.bytes(), text(kPasswords[0]), text(kPasswords[1]));
    test_append_faults_and_retry(initial, base, replacement);
    test_recovery_faults(initial, base, replacement);
    test_authentication_and_transition_rejection(initial, base, replacement);
    test_corruption_stale_snapshots_and_limits(initial, base, replacement);
    test_application_journal_adapter(initial, base, replacement);
    test_application_transport_faults(initial, base, replacement);
    test_full_reserved_reset(initial, base);
    test_application_reset_adapter(base);
    test_read_only_product_entry(base);
    test_v2_login_target(initial, base);
    test_enrollment_login_composition(initial, base);
    test_enrollment_initialization_composition(base);
    test_product_password_ring(initial, base);
    test_product_update_rejections(base);
    test_product_legacy_upgrade_and_append();
    std::cout << "card_record_journal_v2_tests: PASS (10 scenario groups)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
