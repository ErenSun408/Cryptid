#include "card/card_record_journal_v2.h"

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
#include <utility>
#include <vector>

namespace {

using namespace usb_vault::card;
using usb_vault::security::SecureBuffer;
namespace crypto = usb_vault::card::detail;
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

} // namespace

int main() {
  try {
    auto initial = initialize_card_record_and_root_v2(text(kPasswords[0]));
    Memory base;
    fixture(base, initial);
    auto replacement = append_card_password_v2(initial.record.bytes(), text(kPasswords[0]), text(kPasswords[1]));
    test_append_faults_and_retry(initial, base, replacement);
    test_recovery_faults(initial, base, replacement);
    test_authentication_and_transition_rejection(initial, base, replacement);
    test_corruption_stale_snapshots_and_limits(initial, base, replacement);
    std::cout << "card_record_journal_v2_tests: PASS (4 scenario groups)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
