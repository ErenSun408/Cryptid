#include "card/card_record_journal_v1.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

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
using usb_vault::card::commit_card_record_journal_v1;
using usb_vault::card::ICardRecordJournalStorage;
using usb_vault::card::kCardRecordJournalV1Descriptor0Offset;
using usb_vault::card::kCardRecordJournalV1Descriptor1Offset;
using usb_vault::card::kCardRecordJournalV1DescriptorSize;
using usb_vault::card::kCardRecordJournalV1Slot0Offset;
using usb_vault::card::kCardRecordJournalV1Slot1Offset;
using usb_vault::card::kCardRecordJournalV1StorageSize;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
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

[[nodiscard]] bool same(const SecureBuffer &left,
                        const SecureBuffer &right) noexcept {
  return std::ranges::equal(left.bytes(), right.bytes());
}

[[nodiscard]] bool
same_range(const std::array<std::byte, kCardRecordJournalV1StorageSize> &data,
           const std::size_t left, const std::size_t right,
           const std::size_t length) noexcept {
  return std::ranges::equal(std::span(data).subspan(left, length),
                            std::span(data).subspan(right, length));
}

[[nodiscard]] bool same_record_at(
    const std::array<std::byte, kCardRecordJournalV1StorageSize> &data,
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

  std::size_t capacity_{kCardRecordJournalV1StorageSize};
  std::array<std::byte, kCardRecordJournalV1StorageSize> data_{};
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
    // A commit first reads two descriptors and two slots. Corrupt only the
    // immediate staging readback, not the bytes retained by fake storage.
    storage.corrupt_read_call_ = 4;
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
    storage.capacity_ = kCardRecordJournalV1StorageSize - 1;
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

} // namespace

int main() {
  try {
    test_provision_and_alternating_commits();
    test_staging_failures_preserve_active_commit();
    test_descriptor_failures_recover_old_or_complete_new();
    test_staging_cleanup_recovery_can_resume();
    test_corruption_fails_closed_without_credential_rollback();
    test_invalid_inputs_do_not_touch_storage();
    std::cout << "card record journal v1 tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
