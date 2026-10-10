#include "card/card_record_journal_v1.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::CardRecordJournalMediaState;
using usb_vault::card::CardRecordState;
using usb_vault::card::bootstrap_uninitialized_card_record_journal_v1;
using usb_vault::card::commit_card_record_journal_v1;
using usb_vault::card::ICardRecordJournalStorage;
using usb_vault::card::kCardRecordJournalV1Descriptor0Offset;
using usb_vault::card::kCardRecordJournalV1Descriptor1Offset;
using usb_vault::card::kCardRecordJournalV1DescriptorSize;
using usb_vault::card::kCardRecordJournalV1Slot0Offset;
using usb_vault::card::kCardRecordJournalV1Slot1Offset;
using usb_vault::card::kCardRecordJournalV1ReservedSize;
using usb_vault::card::kCardRecordJournalV1StorageSize;
using usb_vault::card::kCardRecordJournalV1ThreeSlot0Offset;
using usb_vault::card::kCardRecordJournalV1ThreeSlot1Offset;
using usb_vault::card::kCardRecordJournalV1ThreeSlotEnd;
using usb_vault::card::kCardRecordV1ThreeSlotSize;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
using usb_vault::card::inspect_card_record;
using usb_vault::card::inspect_card_record_journal_logical_state_v1_read_only;
using usb_vault::card::inspect_card_record_journal_media_v1;
using usb_vault::card::provision_card_record_journal_v1;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::card::read_card_record_journal_v1;
using usb_vault::security::SecureBuffer;

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition)
    fail(message);
}

[[nodiscard]] std::array<std::byte, 32>
sha256(const std::span<const std::byte> input) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    fail("open test SHA-256 provider");
  }
  std::array<std::byte, 32> result{};
  const NTSTATUS status = ::BCryptHash(
      algorithm, nullptr, 0,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR *>(input.data())),
      static_cast<ULONG>(input.size()),
      reinterpret_cast<PUCHAR>(result.data()),
      static_cast<ULONG>(result.size()));
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));
  if (!BCRYPT_SUCCESS(status)) fail("hash test journal bytes");
  return result;
}

template <typename Action>
void expect_card_error(Action &&action, const CardError expected,
                       const char *const message) {
  try {
    action();
  } catch (const std::system_error &error) {
    if (error.code() == make_error_code(expected))
      return;
  }
  fail(message);
}

void store_u32(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] SecureBuffer framed_record(const std::byte fill) {
  SecureBuffer result(kCardRecordV1Size);
  std::ranges::fill(result.bytes(), fill);
  constexpr std::array<std::byte, 8> magic{
      std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
      std::byte{'D'}, std::byte{'2'}, std::byte{},    std::byte{}};
  std::ranges::copy(magic, result.bytes().begin());
  store_u32(result.bytes(), 8, 1);
  store_u32(result.bytes(), 12, static_cast<std::uint32_t>(result.size()));
  store_u32(result.bytes(), 48, 512U * 1024U);
  store_u32(result.bytes(), 52, 2);
  store_u32(result.bytes(), 56, 1);
  store_u32(result.bytes(), 60, 0x13);
  return result;
}

[[nodiscard]] SecureBuffer future_record() {
  SecureBuffer result = framed_record(std::byte{0x47});
  store_u32(result.bytes(), 8, 2);
  return result;
}

[[nodiscard]] bool same(const SecureBuffer &left,
                        const SecureBuffer &right) noexcept {
  return std::ranges::equal(left.bytes(), right.bytes());
}

[[nodiscard]] bool
same_range(const std::array<std::byte, kCardRecordJournalV1ReservedSize> &data,
           const std::size_t left, const std::size_t right,
           const std::size_t length) noexcept {
  return std::ranges::equal(std::span(data).subspan(left, length),
                            std::span(data).subspan(right, length));
}

[[nodiscard]] bool same_record_at(
    const std::array<std::byte, kCardRecordJournalV1ReservedSize> &data,
    const std::size_t offset, const SecureBuffer &record) noexcept {
  return std::ranges::equal(std::span(data).subspan(offset, record.size()),
                            record.bytes());
}

struct WriteFault final {
  std::size_t call{std::numeric_limits<std::size_t>::max()};
  std::size_t bytes_before_failure{};
};

class FakeStorage final : public ICardRecordJournalStorage {
public:
  [[nodiscard]] std::size_t capacity() override { return capacity_; }

  [[nodiscard]] SecureBuffer read(const std::size_t offset,
                                  const std::size_t length) override {
    const std::size_t call = read_calls_++;
    if (offset > data_.size() || length > data_.size() - offset) {
      throw std::system_error(make_error_code(CardError::invalid_operation));
    }
    SecureBuffer result(length);
    std::ranges::copy_n(data_.begin() + offset, length, result.bytes().begin());
    if (call == corrupt_read_call_ && !result.empty()) {
      result.bytes().front() ^= std::byte{1};
    }
    return result;
  }

  void write(const std::size_t offset,
             const std::span<const std::byte> bytes) override {
    const std::size_t call = write_calls_++;
    write_offsets_.push_back(offset);
    if (offset > data_.size() || bytes.size() > data_.size() - offset) {
      throw std::system_error(make_error_code(CardError::invalid_operation));
    }
    std::size_t copied = bytes.size();
    if (call == write_fault_.call) {
      copied = std::min(copied, write_fault_.bytes_before_failure);
    }
    std::ranges::copy_n(bytes.begin(), copied, data_.begin() + offset);
    if (call == write_fault_.call) {
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
  }

  void reset_observation() noexcept {
    read_calls_ = 0;
    write_calls_ = 0;
    write_offsets_.clear();
    write_fault_ = {};
    corrupt_read_call_ = std::numeric_limits<std::size_t>::max();
  }

  std::size_t capacity_{kCardRecordJournalV1ReservedSize};
  std::array<std::byte, kCardRecordJournalV1ReservedSize> data_{};
  WriteFault write_fault_{};
  std::size_t corrupt_read_call_{std::numeric_limits<std::size_t>::max()};
  std::size_t read_calls_{};
  std::size_t write_calls_{};
  std::vector<std::size_t> write_offsets_;
};

void provision(FakeStorage &storage, const SecureBuffer &marker) {
  provision_card_record_journal_v1(storage, marker.bytes());
  storage.reset_observation();
}

void replace_active_record_and_rehash_descriptors(
    FakeStorage &storage, const SecureBuffer &old_record,
    const SecureBuffer &replacement) {
  const std::size_t active_slot =
      same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset,
                     old_record)
          ? kCardRecordJournalV1Slot0Offset
          : kCardRecordJournalV1Slot1Offset;
  std::ranges::copy(replacement.bytes(),
                    storage.data_.begin() + active_slot);

  constexpr std::size_t kRecordDigestOffset = 56;
  constexpr std::size_t kDescriptorDigestOffset = 120;
  const auto record_digest = sha256(replacement.bytes());
  for (const std::size_t descriptor_offset :
       {kCardRecordJournalV1Descriptor0Offset,
        kCardRecordJournalV1Descriptor1Offset}) {
    std::ranges::copy(
        record_digest,
        storage.data_.begin() + descriptor_offset + kRecordDigestOffset);
    const auto descriptor_digest = sha256(
        std::span(storage.data_)
            .subspan(descriptor_offset, kDescriptorDigestOffset));
    std::ranges::copy(
        descriptor_digest,
        storage.data_.begin() + descriptor_offset +
            kDescriptorDigestOffset);
  }
}

void test_media_inspection_is_read_only() {
  FakeStorage storage;
  expect(inspect_card_record_journal_media_v1(storage) ==
             CardRecordJournalMediaState::no_product_evidence,
         "blank journal reported product evidence");
  expect(storage.write_calls_ == 0,
         "media inspection wrote blank storage");

  storage.data_[kCardRecordJournalV1Descriptor0Offset] = std::byte{'U'};
  storage.data_[kCardRecordJournalV1Descriptor0Offset + 1] = std::byte{'V'};
  storage.data_[kCardRecordJournalV1Descriptor0Offset + 2] = std::byte{'J'};
  storage.data_[kCardRecordJournalV1Descriptor0Offset + 3] = std::byte{'R'};
  storage.reset_observation();
  expect(inspect_card_record_journal_media_v1(storage) ==
             CardRecordJournalMediaState::product_evidence,
         "torn descriptor prefix was treated as unclaimed");
  expect(storage.write_calls_ == 0,
         "media inspection normalized damaged product evidence");

  storage.data_.fill(std::byte{});
  storage.data_[kCardRecordJournalV1Slot0Offset] = std::byte{'U'};
  storage.data_[kCardRecordJournalV1Slot0Offset + 1] = std::byte{'V'};
  storage.data_[kCardRecordJournalV1Slot0Offset + 2] = std::byte{'B'};
  storage.data_[kCardRecordJournalV1Slot0Offset + 3] = std::byte{'L'};
  storage.reset_observation();
  expect(inspect_card_record_journal_media_v1(storage) ==
             CardRecordJournalMediaState::product_evidence,
         "partial provisioning marker was treated as resumable");
  expect(storage.write_calls_ == 0,
         "media inspection wrote an interrupted bootstrap");

  storage.data_.fill(std::byte{});
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  std::ranges::copy(marker.bytes(),
                    storage.data_.begin() +
                        kCardRecordJournalV1Slot0Offset);
  storage.reset_observation();
  expect(inspect_card_record_journal_media_v1(storage) ==
             CardRecordJournalMediaState::provisioning_interrupted,
         "complete provisioning marker was not resumable");
}

void test_bootstrap_is_resumable_and_non_destructive() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  FakeStorage partial_marker;
  partial_marker.write_fault_ = {.call = 1, .bytes_before_failure = 4};
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(partial_marker,
                                                        marker.bytes());
      },
      CardError::communication_failed,
      "partial marker bootstrap returned the wrong error");
  partial_marker.reset_observation();
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(partial_marker,
                                                        marker.bytes());
      },
      CardError::device_failure,
      "unprovable partial marker was resumed destructively");
  expect(partial_marker.write_calls_ == 0,
         "partial marker retry wrote damaged product evidence");

  FakeStorage interrupted;
  interrupted.write_fault_ = {.call = 2, .bytes_before_failure = 4};
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(interrupted,
                                                        marker.bytes());
      },
      CardError::communication_failed,
      "partial descriptor bootstrap returned the wrong error");
  expect(inspect_card_record_journal_media_v1(interrupted) ==
             CardRecordJournalMediaState::provisioning_interrupted,
         "complete marker bootstrap lost its resumable evidence");

  interrupted.reset_observation();
  bootstrap_uninitialized_card_record_journal_v1(interrupted,
                                                  marker.bytes());
  SecureBuffer observed = read_card_record_journal_v1(interrupted);
  expect(same(observed, marker),
         "resumed bootstrap did not publish the marker");

  interrupted.reset_observation();
  bootstrap_uninitialized_card_record_journal_v1(interrupted,
                                                  marker.bytes());
  expect(interrupted.write_calls_ == 0,
         "idempotent bootstrap rewrote a valid marker journal");
  expect_card_error(
      [&] {
        provision_card_record_journal_v1(interrupted, marker.bytes());
      },
      CardError::invalid_operation,
      "direct provisioning overwrote existing product evidence");
  expect(interrupted.write_calls_ == 0,
         "rejected direct provisioning touched existing product data");

  SecureBuffer initialized = framed_record(std::byte{0x62});
  commit_card_record_journal_v1(interrupted, initialized.bytes());
  interrupted.reset_observation();
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(interrupted,
                                                        marker.bytes());
      },
      CardError::invalid_operation,
      "bootstrap replaced an initialized record");
  expect(interrupted.write_calls_ == 0,
         "rejected bootstrap wrote an initialized journal");

  const std::size_t initialized_slot =
      same_record_at(interrupted.data_, kCardRecordJournalV1Slot0Offset,
                     initialized)
          ? kCardRecordJournalV1Slot0Offset
          : kCardRecordJournalV1Slot1Offset;
  interrupted.data_[initialized_slot + 73] ^= std::byte{1};
  interrupted.reset_observation();
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(interrupted,
                                                        marker.bytes());
      },
      CardError::device_failure,
      "bootstrap replaced damaged initialized product evidence");
  expect(interrupted.write_calls_ == 0,
         "damaged initialized journal was written by bootstrap");
}

void test_complete_future_record_reaches_product_classification() {
  FakeStorage storage;
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  provision(storage, marker);
  SecureBuffer future = future_record();
  replace_active_record_and_rehash_descriptors(storage, marker, future);
  storage.reset_observation();

  SecureBuffer observed = read_card_record_journal_v1(storage);
  expect(same(observed, future),
         "complete future record was rejected by the physical journal");
  expect(inspect_card_record(observed.bytes()) ==
             CardRecordState::unsupported_version,
         "future record did not reach unsupported-version classification");
  expect(storage.write_calls_ == 0,
         "stable future record was normalized or rewritten");
  storage.reset_observation();
  expect_card_error(
      [&] {
        bootstrap_uninitialized_card_record_journal_v1(storage,
                                                        marker.bytes());
      },
      CardError::invalid_operation,
      "bootstrap replaced a complete future product record");
  expect(storage.write_calls_ == 0,
         "future product record was written by bootstrap");
}

void test_logical_state_inspection_is_strictly_read_only() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();

  {
    FakeStorage storage;
    provision(storage, marker);
    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::provisioned_uninitialized,
           "stable marker journal returned the wrong logical state");
    expect(storage.read_calls_ == 6 && storage.write_calls_ == 0,
           "stable logical-state inspection was not one read-only snapshot");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    SecureBuffer initialized = framed_record(std::byte{0x58});
    commit_card_record_journal_v1(storage, initialized.bytes());
    storage.reset_observation();
    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::initialized_v1,
           "initialized journal returned the wrong logical state");
    expect(storage.read_calls_ == 6 && storage.write_calls_ == 0,
           "initialized logical-state inspection wrote storage");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    SecureBuffer future = future_record();
    replace_active_record_and_rehash_descriptors(storage, marker, future);
    storage.reset_observation();
    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::unsupported_version,
           "future journal did not reach public version classification");
    expect(storage.write_calls_ == 0,
           "future logical-state inspection wrote storage");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    SecureBuffer malformed(kCardRecordV1Size);
    std::ranges::fill(malformed.bytes(), std::byte{0x91});
    replace_active_record_and_rehash_descriptors(storage, marker, malformed);
    storage.reset_observation();
    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::malformed,
           "authenticated malformed framing returned the wrong state");
    expect(storage.write_calls_ == 0,
           "malformed logical-state inspection wrote storage");
  }
}

void test_logical_state_inspection_selects_without_normalizing() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer replacement = framed_record(std::byte{0x69});

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 0, .bytes_before_failure = 37};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "partial staging setup returned the wrong error");
    storage.reset_observation();

    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::provisioned_uninitialized,
           "read-only inspection selected partial staging bytes");
    expect(storage.read_calls_ == 6 && storage.write_calls_ == 0,
           "read-only staging inspection normalized the inactive slot");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 2, .bytes_before_failure = 17};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "published cleanup setup returned the wrong error");
    storage.reset_observation();

    expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
               CardRecordState::initialized_v1,
           "read-only inspection did not select the published commit");
    expect(storage.read_calls_ == 6 && storage.write_calls_ == 0,
           "read-only published-commit inspection normalized storage");
  }
}

void test_logical_state_inspection_fails_closed_without_writes() {
  {
    FakeStorage storage;
    expect_card_error(
        [&] {
          static_cast<void>(
              inspect_card_record_journal_logical_state_v1_read_only(
                  storage));
        },
        CardError::device_failure,
        "blank journal returned a logical record state");
    expect(storage.write_calls_ == 0,
           "blank logical-state inspection wrote storage");
  }

  {
    FakeStorage storage;
    SecureBuffer marker = provision_uninitialized_card_record_v1();
    provision(storage, marker);
    const std::size_t active_slot =
        same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset, marker)
            ? kCardRecordJournalV1Slot0Offset
            : kCardRecordJournalV1Slot1Offset;
    storage.data_[active_slot + 73] ^= std::byte{1};
    storage.reset_observation();
    expect_card_error(
        [&] {
          static_cast<void>(
              inspect_card_record_journal_logical_state_v1_read_only(
                  storage));
        },
        CardError::device_failure,
        "damaged active record returned a logical state");
    expect(storage.write_calls_ == 0,
           "damaged logical-state inspection wrote storage");
  }
}

void test_provision_and_alternating_commits() {
  FakeStorage storage;
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  expect_card_error(
      [&] { static_cast<void>(read_card_record_journal_v1(storage)); },
      CardError::device_failure,
      "blank physical storage was treated as a provisioned card");

  provision_card_record_journal_v1(storage, marker.bytes());
  const std::size_t initial_slot =
      same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset, marker)
          ? kCardRecordJournalV1Slot0Offset
          : kCardRecordJournalV1Slot1Offset;
  const std::size_t initial_inactive_slot =
      initial_slot == kCardRecordJournalV1Slot0Offset
          ? kCardRecordJournalV1Slot1Offset
          : kCardRecordJournalV1Slot0Offset;
  expect(storage.write_offsets_ ==
             std::vector<std::size_t>{initial_inactive_slot, initial_slot,
                                      kCardRecordJournalV1Descriptor0Offset,
                                      kCardRecordJournalV1Descriptor1Offset},
         "provisioning used the wrong physical write order");
  expect(!same_record_at(storage.data_, initial_inactive_slot, marker),
         "provisioning left a second visible marker in the inactive slot");
  expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                    kCardRecordJournalV1Descriptor1Offset,
                    kCardRecordJournalV1DescriptorSize),
         "stable provisioning descriptors were not mirrored");
  constexpr std::size_t kCommitIdOffset = 16;
  constexpr std::size_t kPreviousIdOffset = 32;
  constexpr std::size_t kIdSize = 16;
  std::array<std::byte, kIdSize> provisioning_commit_id{};
  std::ranges::copy_n(storage.data_.begin() +
                          kCardRecordJournalV1Descriptor0Offset +
                          kCommitIdOffset,
                      kIdSize, provisioning_commit_id.begin());
  SecureBuffer observed = read_card_record_journal_v1(storage);
  expect(same(observed, marker), "provisioned marker did not round trip");

  storage.reset_observation();
  SecureBuffer first = framed_record(std::byte{0x31});
  commit_card_record_journal_v1(storage, first.bytes());
  expect(storage.write_offsets_ ==
             std::vector<std::size_t>{
                 initial_inactive_slot, kCardRecordJournalV1Descriptor1Offset,
                 initial_slot, kCardRecordJournalV1Descriptor0Offset},
         "first commit did not use the inactive slot and descriptor");
  expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                    kCardRecordJournalV1Descriptor1Offset,
                    kCardRecordJournalV1DescriptorSize),
         "stable committed descriptors were not mirrored");
  expect(std::ranges::equal(provisioning_commit_id,
                            std::span(storage.data_)
                                .subspan(kCardRecordJournalV1Descriptor0Offset +
                                             kPreviousIdOffset,
                                         kIdSize)),
         "new random commit did not link to the previous commit id");
  expect(
      !std::ranges::equal(
          provisioning_commit_id,
          std::span(storage.data_)
              .subspan(kCardRecordJournalV1Descriptor0Offset + kCommitIdOffset,
                       kIdSize)),
      "record update reused its prior random commit id");
  expect(!same_record_at(storage.data_, initial_slot, marker),
         "first commit retained the visible provisioning history");
  observed = read_card_record_journal_v1(storage);
  expect(same(observed, first), "first committed record was not selected");

  storage.reset_observation();
  SecureBuffer second = framed_record(std::byte{0x52});
  commit_card_record_journal_v1(storage, second.bytes());
  expect(storage.write_offsets_ ==
             std::vector<std::size_t>{
                 initial_slot, kCardRecordJournalV1Descriptor1Offset,
                 initial_inactive_slot, kCardRecordJournalV1Descriptor0Offset},
         "second commit did not alternate back to the initial slot");
  observed = read_card_record_journal_v1(storage);
  expect(same(observed, second), "second committed record was not selected");
}

void test_staging_failures_preserve_active_commit() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer replacement = framed_record(std::byte{0x63});

  {
    FakeStorage storage;
    provision(storage, marker);
    const std::size_t active_slot =
        same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset, marker)
            ? kCardRecordJournalV1Slot0Offset
            : kCardRecordJournalV1Slot1Offset;
    const std::size_t inactive_slot =
        active_slot == kCardRecordJournalV1Slot0Offset
            ? kCardRecordJournalV1Slot1Offset
            : kCardRecordJournalV1Slot0Offset;
    storage.write_fault_ = {.call = 0, .bytes_before_failure = 37};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "partial staging failure returned the wrong error");
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, marker),
           "partial staging write changed the active commit");
    expect(storage.write_offsets_ ==
               std::vector<std::size_t>{inactive_slot, inactive_slot,
                                        kCardRecordJournalV1Descriptor1Offset,
                                        kCardRecordJournalV1Descriptor0Offset},
           "read recovery did not scrub the partially staged record");
    expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                      kCardRecordJournalV1Descriptor1Offset,
                      kCardRecordJournalV1DescriptorSize),
           "staging recovery did not mirror the refreshed filler digest");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 0,
                            .bytes_before_failure = kCardRecordV1Size};
    commit_card_record_journal_v1(storage, replacement.bytes());
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, replacement),
           "complete staging write with error was not resolved by readback");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    // A commit first reads two descriptors and four slots, then over the
    // provisioning marker checks the reserved tail. Corrupt only the staging
    // readback, not the bytes retained by fake storage.
    storage.corrupt_read_call_ = 7;
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::device_failure, "corrupted staging readback was accepted");
    storage.corrupt_read_call_ = std::numeric_limits<std::size_t>::max();
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, marker),
           "unverified staging bytes became the active commit");
  }
}

void test_descriptor_failures_recover_old_or_complete_new() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer replacement = framed_record(std::byte{0x74});

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 1, .bytes_before_failure = 29};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "partial descriptor failure returned the wrong error");
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, marker),
           "partial descriptor write published the staged record");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {
        .call = 1, .bytes_before_failure = kCardRecordJournalV1DescriptorSize};
    commit_card_record_journal_v1(storage, replacement.bytes());
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, replacement),
           "complete descriptor write with error was not recovered as new");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 2, .bytes_before_failure = 17};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "partial old-slot cleanup returned the wrong error");
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, replacement),
           "partial old-slot cleanup hid the already-published commit");
    expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                      kCardRecordJournalV1Descriptor1Offset,
                      kCardRecordJournalV1DescriptorSize),
           "read recovery did not normalize cleanup interruption metadata");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    storage.write_fault_ = {.call = 3, .bytes_before_failure = 17};
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
        CardError::communication_failed,
        "partial descriptor mirror returned the wrong error");
    SecureBuffer observed = read_card_record_journal_v1(storage);
    expect(same(observed, replacement),
           "torn descriptor mirror hid the already-published commit");
    expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                      kCardRecordJournalV1Descriptor1Offset,
                      kCardRecordJournalV1DescriptorSize),
           "read recovery did not normalize the torn descriptor mirror");
  }
}

void test_staging_cleanup_recovery_can_resume() {
  FakeStorage storage;
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer replacement = framed_record(std::byte{0x79});
  provision(storage, marker);

  storage.write_fault_ = {.call = 0, .bytes_before_failure = 37};
  expect_card_error(
      [&] { commit_card_record_journal_v1(storage, replacement.bytes()); },
      CardError::communication_failed,
      "partial staging setup returned the wrong error");

  // Recovery writes filler, then the other descriptor, then the selected
  // descriptor. Tear that final mirror so the next insertion sees two
  // descriptors for one commit with different inactive-slot digests.
  storage.write_fault_ = {.call = 3, .bytes_before_failure = 17};
  expect_card_error(
      [&] { static_cast<void>(read_card_record_journal_v1(storage)); },
      CardError::communication_failed,
      "interrupted staging cleanup returned the wrong error");

  storage.write_fault_ = {};
  SecureBuffer observed = read_card_record_journal_v1(storage);
  expect(same(observed, marker),
         "resumed staging cleanup did not preserve the old commit");
  expect(same_range(storage.data_, kCardRecordJournalV1Descriptor0Offset,
                    kCardRecordJournalV1Descriptor1Offset,
                    kCardRecordJournalV1DescriptorSize),
         "resumed staging cleanup did not restore mirrored descriptors");
}

void test_corruption_fails_closed_without_credential_rollback() {
  FakeStorage storage;
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer replacement = framed_record(std::byte{0x85});
  provision(storage, marker);
  const std::size_t marker_slot =
      same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset, marker)
          ? kCardRecordJournalV1Slot0Offset
          : kCardRecordJournalV1Slot1Offset;
  const std::size_t replacement_slot =
      marker_slot == kCardRecordJournalV1Slot0Offset
          ? kCardRecordJournalV1Slot1Offset
          : kCardRecordJournalV1Slot0Offset;
  commit_card_record_journal_v1(storage, replacement.bytes());

  storage.data_[kCardRecordJournalV1Descriptor0Offset] ^= std::byte{1};
  SecureBuffer observed = read_card_record_journal_v1(storage);
  expect(same(observed, replacement),
         "one damaged descriptor lost the mirrored active commit");

  storage.data_[replacement_slot + 73] ^= std::byte{1};
  expect_card_error(
      [&] { static_cast<void>(read_card_record_journal_v1(storage)); },
      CardError::device_failure,
      "damaged active slot silently rolled credentials backward");

  storage.data_[replacement_slot + 73] ^= std::byte{1};
  storage.data_[kCardRecordJournalV1Descriptor0Offset] ^= std::byte{1};
  storage.data_[kCardRecordJournalV1Descriptor1Offset] ^= std::byte{1};
  expect_card_error(
      [&] { static_cast<void>(read_card_record_journal_v1(storage)); },
      CardError::device_failure,
      "journal with both descriptors damaged did not fail closed");
}

void test_invalid_inputs_do_not_touch_storage() {
  SecureBuffer marker = provision_uninitialized_card_record_v1();
  SecureBuffer malformed(kCardRecordV1Size);

  {
    FakeStorage storage;
    storage.capacity_ = kCardRecordJournalV1ThreeSlotEnd - 1;
    expect_card_error(
        [&] { provision_card_record_journal_v1(storage, marker.bytes()); },
        CardError::invalid_configuration,
        "undersized journal storage was accepted");
    expect(storage.write_calls_ == 0,
           "undersized storage was written before capacity rejection");
  }

  {
    FakeStorage storage;
    expect_card_error(
        [&] { provision_card_record_journal_v1(storage, malformed.bytes()); },
        CardError::invalid_configuration,
        "malformed provisioning record was accepted");
    expect(storage.write_calls_ == 0,
           "malformed provisioning record reached storage");
  }

  {
    FakeStorage storage;
    provision(storage, marker);
    expect_card_error(
        [&] { commit_card_record_journal_v1(storage, malformed.bytes()); },
        CardError::invalid_configuration,
        "malformed replacement record was accepted");
    expect(storage.read_calls_ == 0 && storage.write_calls_ == 0,
           "malformed replacement touched journal storage");
  }
}


void test_reset_preserves_bytes_outside_application_region() {
  class GuardedStorage final : public ICardRecordJournalStorage {
   public:
    std::array<std::byte, kCardRecordJournalV1ReservedSize + 64> data{};
    std::size_t capacity() override { return data.size(); }
    SecureBuffer read(std::size_t offset, std::size_t length) override {
      expect(offset <= data.size() && length <= data.size() - offset, "out-of-range read");
      SecureBuffer result(length);
      std::ranges::copy(std::span(data).subspan(offset, length), result.bytes().begin());
      return result;
    }
    void write(std::size_t offset, std::span<const std::byte> value) override {
      expect(offset <= data.size() && value.size() <= data.size() - offset, "out-of-range write");
      std::ranges::copy(value, data.begin() + offset);
    }
  } storage;
  std::ranges::fill(std::span(storage.data).subspan(kCardRecordJournalV1ReservedSize), std::byte{0xA5});
  const auto marker = provision_uninitialized_card_record_v1();
  provision_card_record_journal_v1(storage, marker.bytes());
  const auto record = framed_record(std::byte{0x37});
  commit_card_record_journal_v1(storage, record.bytes());
  const auto snapshot = usb_vault::card::snapshot_card_record_journal_v1(storage);
  usb_vault::card::reset_card_record_journal_v1(storage, snapshot.bytes());
  expect(std::ranges::all_of(std::span(storage.data).subspan(kCardRecordJournalV1ReservedSize),
      [](std::byte value) { return value == std::byte{0xA5}; }), "reset changed unrelated card data");
}

void test_confirmed_reset_snapshot_and_fault_recovery() {
  using usb_vault::card::snapshot_card_record_journal_v1;
  using usb_vault::card::reset_card_record_journal_v1;
  const auto marker = provision_uninitialized_card_record_v1();
  const auto original = framed_record(std::byte{0x62});
  const auto other = framed_record(std::byte{0x63});
  FakeStorage storage;
  provision(storage, marker);
  commit_card_record_journal_v1(storage, original.bytes());
  storage.reset_observation();
  const auto snapshot = snapshot_card_record_journal_v1(storage);
  expect(same(snapshot, original) && storage.write_calls_ == 0,
         "confirmation snapshot wrote or changed the record");
  expect_card_error([&] { reset_card_record_journal_v1(storage, other.bytes()); },
                    CardError::invalid_operation, "stale confirmation accepted");
  expect(storage.write_calls_ == 0, "stale confirmation wrote storage");
  reset_card_record_journal_v1(storage, snapshot.bytes());
  expect(same(snapshot_card_record_journal_v1(storage), marker),
         "reset did not leave the provisioning marker");
  expect(!same_record_at(storage.data_, kCardRecordJournalV1Slot0Offset, original) &&
         !same_record_at(storage.data_, kCardRecordJournalV1Slot1Offset, original),
         "successful reset retained an old credential slot");
  storage.reset_observation();
  expect_card_error([&] { reset_card_record_journal_v1(storage, snapshot.bytes()); },
                    CardError::invalid_operation, "reset replay accepted");
  expect(storage.write_calls_ == 0, "reset replay wrote storage");

  for (std::size_t call = 0; call < 4; ++call) {
    for (const std::size_t torn : {0U, 75U, 216U}) {
      FakeStorage faulted;
      provision(faulted, marker);
      commit_card_record_journal_v1(faulted, original.bytes());
      faulted.reset_observation();
      faulted.write_fault_ = {call, torn};
      bool succeeded = false;
      try {
        reset_card_record_journal_v1(faulted, original.bytes());
        succeeded = true;
      } catch (const std::system_error&) {}
      const auto before = faulted.data_;
      const auto writes = faulted.write_calls_;
      const auto observed = snapshot_card_record_journal_v1(faulted);
      expect(faulted.data_ == before && faulted.write_calls_ == writes,
             "snapshot normalized a torn journal");
      expect(same(observed, marker) || same(observed, original),
             "torn reset lost both valid states");
      if (succeeded) {
        expect(same(observed, marker), "failed reset reported success");
        expect(!same_record_at(faulted.data_, kCardRecordJournalV1Slot0Offset, original) &&
               !same_record_at(faulted.data_, kCardRecordJournalV1Slot1Offset, original),
               "recovered reset retained old envelopes");
      }
    }
  }
}

// A three-envelope record keeps the two-envelope header, length field included.
[[nodiscard]] SecureBuffer framed_three_slot_record(const std::byte fill) {
  const SecureBuffer header = framed_record(fill);
  SecureBuffer result(kCardRecordV1ThreeSlotSize);
  std::ranges::fill(result.bytes(), fill);
  std::ranges::copy(header.bytes(), result.bytes().begin());
  return result;
}

[[nodiscard]] bool all_zero(
    const std::array<std::byte, kCardRecordJournalV1ReservedSize> &data,
    const std::size_t offset, const std::size_t length) noexcept {
  return std::ranges::all_of(std::span(data).subspan(offset, length),
                             [](const std::byte value) {
                               return value == std::byte{};
                             });
}

[[nodiscard]] bool holds_record_anywhere(const FakeStorage &storage,
                                         const SecureBuffer &record) noexcept {
  for (const std::size_t offset :
       {kCardRecordJournalV1Slot0Offset, kCardRecordJournalV1Slot1Offset,
        kCardRecordJournalV1ThreeSlot0Offset,
        kCardRecordJournalV1ThreeSlot1Offset}) {
    if (offset + record.size() <= storage.data_.size() &&
        same_record_at(storage.data_, offset, record))
      return true;
  }
  return false;
}

void test_length_switch_commits_use_their_own_slots() {
  const auto marker = provision_uninitialized_card_record_v1();
  const auto two = framed_record(std::byte{0x21});
  const auto three = framed_three_slot_record(std::byte{0x31});
  const auto next = framed_three_slot_record(std::byte{0x32});
  FakeStorage storage;
  provision(storage, marker);
  commit_card_record_journal_v1(storage, two.bytes());

  commit_card_record_journal_v1(storage, three.bytes());
  // Checked before any read, so read-side normalization cannot hide it.
  expect(same_record_at(storage.data_, kCardRecordJournalV1ThreeSlot0Offset,
                        three),
         "length switch did not start from the first three-envelope slot");
  expect(all_zero(storage.data_, kCardRecordJournalV1Slot0Offset,
                  kCardRecordJournalV1StorageSize -
                      kCardRecordJournalV1Slot0Offset),
         "length switch left two-envelope slots behind");
  storage.reset_observation();
  expect(same(read_card_record_journal_v1(storage), three) &&
             storage.write_calls_ == 0,
         "completed length switch was not active and stable");
  expect(inspect_card_record_journal_logical_state_v1_read_only(storage) ==
             CardRecordState::initialized_v1,
         "three-envelope record was not classified as initialized");

  commit_card_record_journal_v1(storage, next.bytes());
  expect(same(read_card_record_journal_v1(storage), next),
         "same-length three-envelope commit did not become active");
  expect(same_record_at(storage.data_, kCardRecordJournalV1ThreeSlot1Offset,
                        next) &&
             !holds_record_anywhere(storage, three),
         "same-length commit kept the previous three-envelope record");

  // Returning to the two-envelope length clears the three-envelope slots.
  commit_card_record_journal_v1(storage, two.bytes());
  expect(all_zero(storage.data_, kCardRecordJournalV1ThreeSlot0Offset,
                  kCardRecordJournalV1ThreeSlotEnd -
                      kCardRecordJournalV1ThreeSlot0Offset),
         "switching back left three-envelope slots behind");
  storage.reset_observation();
  expect(same(read_card_record_journal_v1(storage), two) &&
             storage.write_calls_ == 0,
         "switch back was not active and stable");
}

void test_initialization_clears_the_reserved_tail() {
  const auto marker = provision_uninitialized_card_record_v1();
  const auto three = framed_three_slot_record(std::byte{0x41});
  FakeStorage storage;
  provision(storage, marker);
  std::ranges::fill(std::span(storage.data_).subspan(
                        kCardRecordJournalV1StorageSize),
                    std::byte{0x5A});
  commit_card_record_journal_v1(storage, three.bytes());
  expect(same(read_card_record_journal_v1(storage), three),
         "initialization from the marker did not become active");
  expect(all_zero(storage.data_, kCardRecordJournalV1ThreeSlotEnd,
                  kCardRecordJournalV1ReservedSize -
                      kCardRecordJournalV1ThreeSlotEnd),
         "initialization kept data after the three-envelope slots");
  expect(all_zero(storage.data_, kCardRecordJournalV1Slot0Offset,
                  kCardRecordJournalV1StorageSize -
                      kCardRecordJournalV1Slot0Offset),
         "initialization kept the provisioning marker slots");
}

void test_interrupted_length_switch_recovers_one_record() {
  const auto marker = provision_uninitialized_card_record_v1();
  const auto two = framed_record(std::byte{0x51});
  const auto three = framed_three_slot_record(std::byte{0x52});
  for (std::size_t call = 0; call < 6; ++call) {
    for (const std::size_t torn : {0U, 64U, 216U}) {
      FakeStorage storage;
      provision(storage, marker);
      commit_card_record_journal_v1(storage, two.bytes());
      storage.reset_observation();
      storage.write_fault_ = {call, torn};
      try {
        commit_card_record_journal_v1(storage, three.bytes());
      } catch (const std::system_error &) {
      }
      storage.reset_observation();
      const auto observed = read_card_record_journal_v1(storage);
      const bool old_active = same(observed, two);
      expect(old_active || same(observed, three),
             "interrupted length switch lost both records");
      expect(!holds_record_anywhere(storage, old_active ? three : two),
             "recovered journal kept the inactive record");
      expect(old_active
                 ? all_zero(storage.data_, kCardRecordJournalV1ThreeSlot0Offset,
                            kCardRecordJournalV1ThreeSlotEnd -
                                kCardRecordJournalV1ThreeSlot0Offset)
                 : all_zero(storage.data_, kCardRecordJournalV1Slot0Offset,
                            kCardRecordJournalV1StorageSize -
                                kCardRecordJournalV1Slot0Offset),
             "recovered journal kept bytes of the other record length");
      storage.reset_observation();
      static_cast<void>(read_card_record_journal_v1(storage));
      expect(storage.write_calls_ == 0, "recovered journal was not stable");
    }
  }
}

void test_reset_clears_any_length_and_earlier_formats() {
  using usb_vault::card::reset_card_record_journal_v1;
  using usb_vault::card::snapshot_card_record_journal_v1;
  const auto marker = provision_uninitialized_card_record_v1();

  FakeStorage three_slot;
  provision(three_slot, marker);
  const auto three = framed_three_slot_record(std::byte{0x61});
  commit_card_record_journal_v1(three_slot, three.bytes());
  reset_card_record_journal_v1(three_slot, three.bytes());
  expect(same(snapshot_card_record_journal_v1(three_slot), marker) &&
             all_zero(three_slot.data_, kCardRecordJournalV1StorageSize,
                      kCardRecordJournalV1ReservedSize -
                          kCardRecordJournalV1StorageSize),
         "three-envelope reset did not restore a provisioned card");

  // An earlier format: a future-version record with its own data after the
  // two-envelope layout. Reading leaves that data alone; reset removes it.
  FakeStorage earlier;
  provision(earlier, marker);
  const auto future = future_record();
  replace_active_record_and_rehash_descriptors(earlier, marker, future);
  std::ranges::fill(std::span(earlier.data_).subspan(
                        kCardRecordJournalV1StorageSize),
                    std::byte{0x77});
  earlier.reset_observation();
  expect(same(read_card_record_journal_v1(earlier), future) &&
             earlier.write_calls_ == 0,
         "reading an earlier format changed its data");
  expect_card_error(
      [&] {
        commit_card_record_journal_v1(earlier,
                                      framed_record(std::byte{0x62}).bytes());
      },
      CardError::invalid_configuration,
      "an ordinary commit replaced an earlier-format record");
  reset_card_record_journal_v1(earlier, future.bytes());
  expect(same(snapshot_card_record_journal_v1(earlier), marker) &&
             all_zero(earlier.data_, kCardRecordJournalV1StorageSize,
                      kCardRecordJournalV1ReservedSize -
                          kCardRecordJournalV1StorageSize),
         "earlier-format reset did not restore a provisioned card");
}

} // namespace

int main() {
  try {
    test_length_switch_commits_use_their_own_slots();
    test_initialization_clears_the_reserved_tail();
    test_interrupted_length_switch_recovers_one_record();
    test_reset_clears_any_length_and_earlier_formats();
    test_media_inspection_is_read_only();
    test_bootstrap_is_resumable_and_non_destructive();
    test_complete_future_record_reaches_product_classification();
    test_logical_state_inspection_is_strictly_read_only();
    test_logical_state_inspection_selects_without_normalizing();
    test_logical_state_inspection_fails_closed_without_writes();
    test_provision_and_alternating_commits();
    test_staging_failures_preserve_active_commit();
    test_descriptor_failures_recover_old_or_complete_new();
    test_staging_cleanup_recovery_can_resume();
    test_corruption_fails_closed_without_credential_rollback();
    test_invalid_inputs_do_not_touch_storage();
    test_confirmed_reset_snapshot_and_fault_recovery();
    test_reset_preserves_bytes_outside_application_region();
    std::cout << "card record journal v1 tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
