#include "card/card_record_journal_v2.h"
#include "card/vikey/vikey_card_journal_store.h"
#include "card/vikey/card_operation_lease.h"

#include "card/card_record_crypto.h"
#include "card/card_record_journal_v1_internal.h"
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

void provision(Memory& storage, const SecureBuffer& legacy) {
  auto marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(storage, marker.bytes());
  commit_card_record_journal_v1(storage, legacy.bytes());
  storage.online();
}

void expect_password(const SecureBuffer& record, const std::string_view password,
    const SecureBuffer& root) {
  auto found = unlock_card_record_v2(record.bytes(), text(password));
  expect(same(found.bytes(), root.bytes()), "ring password changed root");
}

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

void replace_legacy_anchor_and_rehash(Memory& storage, const SecureBuffer& anchor) {
  const auto selected_slot = crypto::card_journal_u32(std::span(storage.data), 48);
  const auto offset = selected_slot == 0 ? 304U : 520U;
  std::ranges::copy(anchor.bytes(), storage.data.begin() + offset);
  auto digest = sha256(anchor.bytes());
  for (const auto descriptor : {0U, 152U}) {
    std::ranges::copy(digest, storage.data.begin() + descriptor + 56);
    auto checksum = sha256(std::span(storage.data).subspan(descriptor, 120));
    std::ranges::copy(checksum, storage.data.begin() + descriptor + 120);
  }
}

class UpgradeTransport final : public vikey::IVikeyCardJournalTransport {
 public:
  explicit UpgradeTransport(const Memory& source) : media(source) {}
  SecureBuffer snapshot_reserved() override {
    require_lease();
    if (media.offline) throw std::system_error(make_error_code(CardError::communication_failed));
    return copy(std::span(media.data).first(1808));
  }
  void write_block(const SecureBuffer& expected, std::size_t offset, const SecureBuffer& block) override {
    require_lease();
    if (!same(expected.bytes(), std::span(media.data).first(1808))) {
      throw std::system_error(make_error_code(CardError::invalid_operation));
    }
    media.write(offset, block.bytes());
  }
  Memory media;
 private:
  void require_lease() {
    bool blocked = false;
    std::thread competitor([&] {
      try { vikey::CardOperationLease lease; }
      catch (const std::system_error& error) { blocked = error.code().value() == ERROR_BUSY; }
    });
    competitor.join();
    expect(blocked, "migration transport call escaped whole operation lease");
  }
};

void test_application_upgrade_adapter(const InitializedCardRecordV1& initial,
    const SecureBuffer& legacy, const Memory& base) {
  UpgradeTransport transport(base);
  vikey::VikeyCardJournalStore store(transport);
  expect(same(store.snapshot_legacy().bytes(), legacy.bytes()), "upgrade adapter changed legacy selection");
  auto prepared = store.prepare_upgrade(legacy.bytes(), text(kPasswords[0]));
  expect(transport.media.writes == 0, "upgrade preparation wrote physical storage");
  store.commit_upgrade(std::move(prepared));
  const auto record = store.snapshot();
  expect_password(record, kPasswords[0], initial.root_secret);
  expect_password(record, kPasswords[1], initial.root_secret);
  expect(same(record.bytes().subspan(32, 64), legacy.bytes().first(64)) &&
         same(record.bytes().subspan(96, 152), legacy.bytes().subspan(64, 152)),
         "application upgrade changed retained context or envelopes");
  const auto writes = transport.media.writes;
  rejects<std::system_error>([&] { store.commit_upgrade(std::move(prepared)); },
      "consumed upgrade preparation was replayed");
  rejects<std::system_error>([&] { static_cast<void>(store.snapshot_legacy()); },
      "published application upgrade fell back to legacy selection");
  expect(transport.media.writes == writes, "consumed or published legacy rejection wrote card");
  transport.media.sentinel();

  UpgradeTransport first(base), other(base);
  vikey::VikeyCardJournalStore first_store(first), other_store(other);
  auto bound = first_store.prepare_upgrade(legacy.bytes(), text(kPasswords[0]));
  rejects<std::system_error>([&] { other_store.commit_upgrade(std::move(bound)); },
      "upgrade preparation accepted another card/store with identical bytes");
  rejects<std::system_error>([&] { first_store.commit_upgrade(std::move(bound)); },
      "rejected cross-store attempt remained reusable");
  expect(first.media.writes == 0 && other.media.writes == 0, "cross-store upgrade rejection wrote card");
  auto expired = first_store.prepare_upgrade(legacy.bytes(), text(kPasswords[1]));
  for (const auto offset : {0U, 152U}) {
    first.media.data[offset + 16] ^= std::byte{1};
    auto digest = sha256(std::span(first.media.data).subspan(offset, 120));
    std::ranges::copy(digest, first.media.data.begin() + offset + 120);
  }
  rejects<std::system_error>([&] { first_store.commit_upgrade(std::move(expired)); },
      "upgrade preparation survived a changed source commit");
  expect(first.media.writes == 0, "expired application upgrade wrote card");

  for (std::size_t call = 0; call < 12; ++call) {
    UpgradeTransport interrupted(base);
    vikey::VikeyCardJournalStore interrupted_store(interrupted);
    auto attempt = interrupted_store.prepare_upgrade(legacy.bytes(), text(kPasswords[0]));
    interrupted.media.cut_write = call;
    interrupted.media.tear_mode = 2;
    rejects<std::system_error>([&] { interrupted_store.commit_upgrade(std::move(attempt)); },
        "interrupted application migration reported completion");
    expect(interrupted.media.writes == call + 1, "application upgrade retried an indeterminate write");
    interrupted.media.online();
    auto visible = snapshot_card_record_journal_v1(interrupted.media);
    if (crypto::is_card_record_v2_anchor(visible.bytes())) {
      auto selected = interrupted_store.snapshot();
      auto root = unlock_card_record_v2(selected.bytes(), text(kPasswords[1]));
      interrupted_store.recover(selected.bytes(), root);
    } else {
      auto fresh = interrupted_store.prepare_upgrade(legacy.bytes(), text(kPasswords[1]));
      interrupted_store.commit_upgrade(std::move(fresh));
    }
    expect(same(interrupted_store.snapshot().bytes().subspan(96, 152), legacy.bytes().subspan(64, 152)),
           "application restart changed old encrypted envelopes");
    interrupted.media.sentinel();
  }
}

void test_migration_faults_and_restart() {
  auto initial = initialize_card_record_and_root_v1(text(kPasswords[0]));
  auto legacy = rewrap_card_record_v1(initial.record.bytes(), text(kPasswords[0]),
      text(kPasswords[1]), text(kPasswords[0]));
  Memory base;
  provision(base, legacy);
  test_application_upgrade_adapter(initial, legacy, base);
  auto plan = prepare_card_journal_v2_migration(base, legacy.bytes(), text(kPasswords[0]));
  expect(base.writes == 0, "migration preparation wrote storage");
  Memory completed = base;
  commit_card_journal_v2_migration(completed, plan);
  const auto write_count = completed.writes;
  expect(write_count == 12, "unexpected initial migration write count");
  auto new_record = snapshot_card_record_journal_v2(completed);
  expect(same(new_record.bytes(), plan.replacement_record()), "migration published wrong record");
  expect_password(new_record, kPasswords[0], initial.root_secret);
  expect_password(new_record, kPasswords[1], initial.root_secret);
  completed.sentinel();

  std::size_t restarted = 0;
  for (std::size_t call = 0; call < write_count; ++call) {
    for (const std::size_t mode : {0U, 2U, 1U}) {
      Memory interrupted = base;
      interrupted.cut_write = call;
      interrupted.tear_mode = mode;
      rejects<std::system_error>([&] { commit_card_journal_v2_migration(interrupted, plan); },
          "simulated power loss did not interrupt migration");
      interrupted.online();
      auto visible = snapshot_card_record_journal_v1(interrupted);
      if (crypto::is_card_record_v2_anchor(visible.bytes())) {
        auto selected = snapshot_card_record_journal_v2(interrupted);
        expect(same(selected.bytes(), plan.replacement_record()),
            "published migration fell back to legacy bytes");
      } else {
        expect(same(visible.bytes(), legacy.bytes()), "unpublished migration lost legacy credentials");
        Memory fresh_process = interrupted;
        auto fresh_plan = prepare_card_journal_v2_migration(fresh_process, legacy.bytes(), text(kPasswords[1]));
        commit_card_journal_v2_migration(fresh_process, fresh_plan);
        auto fresh_record = snapshot_card_record_journal_v2(fresh_process);
        expect(same(fresh_record.bytes().subspan(32, 64), legacy.bytes().first(64)) &&
            same(fresh_record.bytes().subspan(96, 152), legacy.bytes().subspan(64, 152)),
            "restart changed a retained legacy envelope or context");
        verify_card_record_journal_v2_anchor(fresh_process, fresh_record.bytes(), initial.root_secret);
        fresh_process.sentinel();
        ++restarted;
      }
      commit_card_journal_v2_migration(interrupted, plan);
      auto selected = snapshot_card_record_journal_v2(interrupted);
      expect(same(selected.bytes(), plan.replacement_record()), "migration retry changed its record");
      verify_card_record_journal_v2_anchor(interrupted, selected.bytes(), initial.root_secret);
      expect(!same(std::span(interrupted.data).subspan(304, 216), legacy.bytes()) &&
          !same(std::span(interrupted.data).subspan(520, 216), legacy.bytes()),
          "migration left a standalone legacy credential copy");
      interrupted.sentinel();
    }
  }

  // Lose all volatile plan material after the first extension write tears.
  Memory restart = base;
  restart.cut_write = 4;
  restart.tear_mode = 2;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(restart, plan); }, "missing staging interruption");
  restart.online();
  auto resumed = prepare_card_journal_v2_migration(restart, legacy.bytes(), text(kPasswords[1]));
  commit_card_journal_v2_migration(restart, resumed);
  expect_password(snapshot_card_record_journal_v2(restart), kPasswords[0], initial.root_secret);
  restart.sentinel();

  Memory published = base;
  published.cut_write = 10; // Anchor published; old credential cleanup pending.
  published.tear_mode = 0;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(published, plan); },
      "missing legacy cleanup interruption");
  published.online();
  for (std::size_t call = 0; call < 3; ++call) {
    for (const std::size_t mode : {0U, 2U, 1U}) {
      Memory interrupted = published;
      interrupted.cut_write = call;
      interrupted.tear_mode = mode;
      rejects<std::system_error>([&] {
        recover_card_record_journal_v2(interrupted, plan.replacement_record(), initial.root_secret);
      }, "legacy cleanup power loss did not interrupt recovery");
      interrupted.online();
      expect(same(snapshot_card_record_journal_v2(interrupted).bytes(), plan.replacement_record()),
          "legacy cleanup recovery rolled back the published anchor");
      verify_card_record_journal_v2_anchor(interrupted, plan.replacement_record(), initial.root_secret);
      recover_card_record_journal_v2(interrupted, plan.replacement_record(), initial.root_secret);
      interrupted.sentinel();
    }
  }

  // A complete write that reports failure is accepted only after exact readback.
  Memory reported = base;
  reported.report_error_after_write = 4;
  commit_card_journal_v2_migration(reported, plan);
  expect(same(snapshot_card_record_journal_v2(reported).bytes(), plan.replacement_record()),
      "verified complete write was incorrectly lost");
  auto three = append_card_password_v2(new_record.bytes(), text(kPasswords[1]), text(kPasswords[2]));
  commit_card_record_journal_v2(completed, new_record.bytes(), three.bytes(), initial.root_secret);
  auto four = append_card_password_v2(three.bytes(), text(kPasswords[0]), text(kPasswords[3]));
  commit_card_record_journal_v2(completed, three.bytes(), four.bytes(), initial.root_secret);
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(unlock_card_record_v2(four.bytes(), text(kPasswords[0])));
  }, "fourth password did not evict the oldest retained legacy password");
  for (const auto i : {1U, 2U, 3U}) expect_password(four, kPasswords[i], initial.root_secret);
  completed.sentinel();
  std::cout << "migration write faults: " << write_count * 3 << "\n";
  std::cout << "legacy cleanup recovery faults: 9\n";
  std::cout << "unpublished restart checks: " << restarted << "\n";
}

void test_unowned_data_capacity_short_reads_and_expired_migration() {
  auto initial = initialize_card_record_and_root_v1(text(kPasswords[0]));
  Memory base;
  provision(base, initial.record);
  auto plan = prepare_card_journal_v2_migration(base, initial.record.bytes(), text(kPasswords[0]));
  Memory foreign = base;
  foreign.data[900] = std::byte{0x44};
  rejects<std::system_error>([&] {
    static_cast<void>(prepare_card_journal_v2_migration(foreign, initial.record.bytes(), text(kPasswords[0])));
  }, "unowned extension accepted during preparation");
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(foreign, plan); },
      "extension changed after preparation was overwritten");
  expect(foreign.writes == 0 && foreign.data[900] == std::byte{0x44}, "foreign extension changed");
  Memory small = base;
  small.reported_capacity = 1807;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(small, plan); },
      "insufficient capacity accepted");
  expect(small.writes == 0, "insufficient capacity wrote storage");
  Memory short_read = base;
  short_read.short_read = 0;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(short_read, plan); },
      "short source read accepted");
  expect(short_read.writes == 0, "short source read wrote storage");
  Memory completed = base;
  commit_card_journal_v2_migration(completed, plan);
  Memory reserved = base;
  reserved.cut_write = 4;
  reserved.tear_mode = 0;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(reserved, plan); },
      "missing migration reservation interruption");
  reserved.online();
  // A complete foreign descriptor must survive rejection even if its record
  // digest does not match: damage does not turn foreign evidence into ours.
  std::ranges::copy(std::span(completed.data).subspan(736, 152),
      reserved.data.begin() + 736);
  reserved.data[736 + 32] ^= std::byte{1};
  auto checksum = sha256(std::span(reserved.data).subspan(736, 120));
  std::ranges::copy(checksum, reserved.data.begin() + 736 + 120);
  auto foreign_descriptor = reserved.data;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(reserved, plan); },
      "complete foreign staging descriptor was overwritten");
  expect(reserved.writes == 0 && reserved.data == foreign_descriptor,
      "foreign staging descriptor rejection wrote storage");
  crypto::card_journal_store_u32(std::span(reserved.data), 736 + 8, 9);
  checksum = sha256(std::span(reserved.data).subspan(736, 120));
  std::ranges::copy(checksum, reserved.data.begin() + 736 + 120);
  auto future_descriptor = reserved.data;
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(reserved, plan); },
      "complete future staging descriptor was overwritten");
  expect(reserved.writes == 0 && reserved.data == future_descriptor,
      "future staging descriptor rejection wrote storage");
  for (const bool future : {false, true}) {
    Memory pending_cleanup = base;
    pending_cleanup.cut_write = 2; // Durable intent, old-region cleanup pending.
    pending_cleanup.tear_mode = 0;
    rejects<std::system_error>([&] { commit_card_journal_v2_migration(pending_cleanup, plan); },
        "missing interrupted reservation cleanup");
    pending_cleanup.online();
    const auto& evidence = future ? future_descriptor : foreign_descriptor;
    std::ranges::copy(std::span(evidence).subspan(736, 152), pending_cleanup.data.begin() + 736);
    const auto before = pending_cleanup.data;
    rejects<std::system_error>([&] { commit_card_journal_v2_migration(pending_cleanup, plan); },
        "foreign/future staging was accepted before pending legacy cleanup");
    expect(pending_cleanup.writes == 0 && pending_cleanup.data == before,
        "foreign/future evidence triggered legacy cleanup before rejection");
  }
  Memory stale = base;
  auto changed = rewrap_card_record_v1(initial.record.bytes(), text(kPasswords[0]),
      text(kPasswords[1]), text(kPasswords[0]));
  commit_card_record_journal_v1(stale, changed.bytes());
  stale.online();
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(stale, plan); },
      "expired migration snapshot accepted");
  expect(stale.writes == 0, "expired migration wrote storage");
}

void test_single_password_and_legacy_client_rejection() {
  auto initial = initialize_card_record_and_root_v1(text(kPasswords[0]));
  Memory storage;
  provision(storage, initial.record);
  rejects<CardCredentialRejected>([&] {
    static_cast<void>(prepare_card_journal_v2_migration(storage, initial.record.bytes(), text("wrong-password")));
  }, "migration preparation accepted the wrong old password");
  expect(storage.writes == 0, "failed credential preparation wrote storage");
  auto plan = prepare_card_journal_v2_migration(storage, initial.record.bytes(), text(kPasswords[0]));
  auto moved = std::move(plan);
  rejects<std::system_error>([&] { commit_card_journal_v2_migration(storage, plan); },
      "moved-from preparation was accepted");
  expect(storage.writes == 0, "moved-from plan wrote storage");
  commit_card_journal_v2_migration(storage, moved);
  auto record = snapshot_card_record_journal_v2(storage);
  expect(same(record.bytes().subspan(32, 64), initial.record.bytes().first(64)) &&
      same(record.bytes().subspan(96, 152), initial.record.bytes().subspan(64, 152)),
      "single-password migration altered unknown legacy envelopes");
  expect_password(record, kPasswords[0], initial.root_secret);
  auto anchor = snapshot_card_record_journal_v1(storage);
  expect(inspect_card_record(anchor.bytes()) == CardRecordState::unsupported_version,
      "legacy inspection accepted the published anchor");
  storage.online();
  rejects<std::system_error>([&] { commit_card_record_journal_v1(storage, initial.record.bytes()); },
      "legacy writer accepted the migrated card");
  rejects<std::system_error>([&] { reset_card_record_journal_v1(storage, initial.record.bytes()); },
      "legacy reset accepted the migrated card");
  auto marker = provision_uninitialized_card_record_v1();
  rejects<std::system_error>([&] { bootstrap_uninitialized_card_record_journal_v1(storage, marker.bytes()); },
      "legacy bootstrap accepted the migrated card");
  rejects<std::system_error>([&] { provision_card_record_journal_v1(storage, marker.bytes()); },
      "legacy provisioning accepted the migrated card");
  expect(storage.writes == 0, "legacy rejection wrote storage");
  // A new process no longer has a preparation, but can authenticate and repair
  // the published format. It must never retry the old upgrade against it.
  auto fresh_root = unlock_card_record_v2(record.bytes(), text(kPasswords[0]));
  recover_card_record_journal_v2(storage, record.bytes(), fresh_root);
  rejects<std::system_error>([&] {
    static_cast<void>(prepare_card_journal_v2_migration(storage, initial.record.bytes(), text(kPasswords[0])));
  }, "published card was reinterpreted as an old migration source");
  expect(storage.writes == 0, "healthy published reopening wrote storage");
  Memory forged = storage;
  auto changed_anchor = copy(anchor.bytes());
  changed_anchor.bytes()[70] ^= std::byte{1};
  replace_legacy_anchor_and_rehash(forged, changed_anchor);
  rejects<CardCredentialRejected>([&] {
    recover_card_record_journal_v2(forged, record.bytes(), fresh_root);
  }, "forged published anchor permitted recovery");
  expect(forged.writes == 0, "forged published anchor wrote storage");
  Memory missing = storage;
  std::ranges::fill(std::span(missing.data).subspan(736, 1072), std::byte{});
  rejects<std::system_error>([&] { static_cast<void>(snapshot_card_record_journal_v2(missing)); },
      "missing published extension fell back to old credentials");
  expect(missing.writes == 0, "missing extension triggered writes");
  storage.sentinel();
}

void test_initialization_publication_and_restart() {
  Memory base;
  auto marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(base, marker.bytes());
  base.online();
  auto plan = prepare_card_journal_v2_initialization(base, marker.bytes(), text(kPasswords[0]));
  expect(base.writes == 0, "initialization preparation wrote card");
  Memory completed = base;
  auto root = commit_card_journal_v2_initialization(completed, plan);
  auto record = snapshot_card_record_journal_v2(completed);
  expect(completed.writes == 12, "initialization publication changed write count");
  expect_password(record, kPasswords[0], root);
  verify_card_record_journal_v2_anchor(completed, record.bytes(), root);
  completed.sentinel();

  for (std::size_t call = 0; call < 12; ++call) {
    for (const std::size_t mode : {0U, 2U, 1U}) {
      Memory interrupted = base;
      interrupted.cut_write = call;
      interrupted.tear_mode = mode;
      rejects<std::system_error>([&] {
        static_cast<void>(commit_card_journal_v2_initialization(interrupted, plan));
      }, "initialization power loss reported completion");
      interrupted.online();
      auto resumed_root = commit_card_journal_v2_initialization(interrupted, plan);
      expect(same(resumed_root.bytes(), root.bytes()), "retained initialization changed root");
      expect(same(snapshot_card_record_journal_v2(interrupted).bytes(), record.bytes()),
          "initialization retry changed logical record");
      interrupted.sentinel();
    }
  }
  // Filler-only staging cannot recover a lost root; a complete record can.
  for (const std::size_t mode : {2U, 1U}) {
    Memory restart = base;
    restart.cut_write = mode == 2 ? 4 : 5;
    restart.tear_mode = mode;
    rejects<std::system_error>([&] {
      static_cast<void>(commit_card_journal_v2_initialization(restart, plan));
    }, "missing initialization staging interruption");
    restart.online();
    if (mode == 2) {
      const auto before = restart.data;
      rejects<std::system_error>([&] {
        static_cast<void>(prepare_card_journal_v2_initialization(restart, marker.bytes(), text(kPasswords[0])));
      }, "lost initialization root guessed from incomplete credential");
      expect(restart.writes == 0 && restart.data == before, "incomplete initialization was overwritten");
    } else {
      rejects<CardCredentialRejected>([&] {
        static_cast<void>(prepare_card_journal_v2_initialization(restart, marker.bytes(), text("wrong-password")));
      }, "staged initialization accepted wrong password");
      auto fresh = prepare_card_journal_v2_initialization(restart, marker.bytes(), text(kPasswords[0]));
      auto restored = commit_card_journal_v2_initialization(restart, fresh);
      expect(same(restored.bytes(), root.bytes()), "process restart changed staged initialization root");
      expect(same(snapshot_card_record_journal_v2(restart).bytes(), record.bytes()),
          "process restart changed staged credential record");
    }
    restart.sentinel();
  }
  Memory foreign = base;
  std::ranges::copy(record.bytes(), foreign.data.begin() + 1040);
  const auto before = foreign.data;
  rejects<std::system_error>([&] {
    static_cast<void>(prepare_card_journal_v2_initialization(foreign, marker.bytes(), text(kPasswords[0])));
  }, "unreserved complete initialization credential was accepted");
  expect(foreign.writes == 0 && foreign.data == before, "foreign initialization evidence changed");
  completed.online();
  rejects<std::system_error>([&] { commit_card_record_journal_v1(completed, marker.bytes()); },
      "old client overwrote directly initialized card");
  expect(completed.writes == 0, "old client rejection wrote initialized card");
  std::cout << "initialization publication faults: 36; lost-plan checks: 2\n";
}

class AcceptedPasswordPolicy final : public usb_vault::security::IPasswordStrengthPolicy {
 public:
  usb_vault::security::PasswordStrength assess(const SecureBuffer&) const noexcept override {
    return usb_vault::security::PasswordStrength::acceptable;
  }
};

void test_application_initialization_adapter() {
  Memory base;
  auto marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(base, marker.bytes());
  base.online();
  AcceptedPasswordPolicy policy;
  UpgradeTransport transport(base), other(base);
  vikey::VikeyCardJournalStore store(transport), other_store(other);
  const auto snapshot = store.snapshot_provisioning();
  auto confirmed = usb_vault::security::confirm_password(policy, text(kPasswords[0]), text(kPasswords[0]));
  auto plan = store.prepare_initialization(snapshot.bytes(), std::move(confirmed));
  expect(transport.media.writes == 0, "application initialization preparation wrote card");
  rejects<std::system_error>([&] { static_cast<void>(other_store.initialize(plan)); },
      "initialization plan accepted another store");
  expect(other.media.writes == 0, "cross-store initialization wrote card");
  transport.media.cut_write = 4;
  transport.media.tear_mode = 2;
  rejects<std::system_error>([&] { static_cast<void>(store.initialize(plan)); },
      "application initialization accepted partial postimage");
  expect(transport.media.writes == 5, "application initialization retried uncertain write");
  transport.media.online();
  auto root = store.initialize(plan);
  auto selected = store.snapshot();
  expect_password(selected, kPasswords[0], root);
  auto repeated = store.initialize(plan);
  expect(same(repeated.bytes(), root.bytes()), "verified initialization retry changed root");
  rejects<std::system_error>([&] { static_cast<void>(store.snapshot_provisioning()); },
      "published initialization was exposed as a provisioning marker");
  transport.media.sentinel();
}

} // namespace

int main() {
  try {
    test_migration_faults_and_restart();
    test_unowned_data_capacity_short_reads_and_expired_migration();
    test_single_password_and_legacy_client_rejection();
    test_initialization_publication_and_restart();
    test_application_initialization_adapter();
    std::cout << "card_record_journal_v2_migration_tests: PASS (migration and initialization)\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
