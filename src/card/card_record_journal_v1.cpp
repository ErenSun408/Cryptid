#include "card/card_record_journal_v1.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <system_error>

namespace usb_vault::card {
namespace {

constexpr std::array<std::byte, 8> kDescriptorMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'J'}, std::byte{'R'},
    std::byte{'N'}, std::byte{'2'}, std::byte{},    std::byte{}};
constexpr std::array<std::byte, 4> kDescriptorPrefix{
    std::byte{'U'}, std::byte{'V'}, std::byte{'J'}, std::byte{'R'}};
constexpr std::array<std::byte, 4> kProvisioningPrefix{
    std::byte{'U'}, std::byte{'V'}, std::byte{'B'}, std::byte{'L'}};
constexpr std::array<std::byte, 4> kInitializedPrefix{
    std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'}};
constexpr std::uint32_t kDescriptorVersion = 1;
constexpr std::size_t kDescriptorPrefixSize = 120;
constexpr std::size_t kDigestSize = 32;
constexpr std::size_t kCommitIdSize = 16;
constexpr std::size_t kDescriptorCommitIdOffset = 16;
constexpr std::size_t kDescriptorPreviousIdOffset = 32;
constexpr std::size_t kDescriptorSlotOffset = 48;
constexpr std::size_t kDescriptorRecordLengthOffset = 52;
constexpr std::size_t kDescriptorRecordDigestOffset = 56;
constexpr std::size_t kDescriptorInactiveDigestOffset = 88;
constexpr std::size_t kDescriptorDigestOffset = 120;

using Digest = std::array<std::byte, kDigestSize>;
using CommitId = std::array<std::byte, kCommitIdSize>;
using DescriptorBytes =
    std::array<std::byte, kCardRecordJournalV1DescriptorSize>;

template <typename Handle, NTSTATUS(WINAPI *Close)(Handle, ULONG)>
class UniqueHandle final {
public:
  UniqueHandle() noexcept = default;
  ~UniqueHandle() noexcept { reset(); }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  [[nodiscard]] Handle get() const noexcept { return value_; }
  [[nodiscard]] Handle *put() noexcept {
    reset();
    return &value_;
  }

private:
  void reset() noexcept {
    if (value_ != nullptr) {
      static_cast<void>(Close(value_, 0));
      value_ = nullptr;
    }
  }

  Handle value_{};
};

using AlgorithmHandle =
    UniqueHandle<BCRYPT_ALG_HANDLE, ::BCryptCloseAlgorithmProvider>;

struct ParsedDescriptor final {
  bool valid{};
  bool record_matches{};
  bool inactive_matches{};
  CommitId commit_id{};
  CommitId previous_id{};
  std::uint32_t slot{};
};

struct LoadedJournal final {
  security::SecureBuffer record;
  CommitId commit_id{};
  CommitId previous_id{};
  std::uint32_t slot{};
  std::uint32_t descriptor{};
  bool mirrored{};
};

[[noreturn]] void fail(const CardError error, const char *const context) {
  throw std::system_error(make_error_code(error), context);
}

void require_nt_success(const NTSTATUS status, const char *const context) {
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            context);
  }
}

void store_u32(const std::span<std::byte> destination, const std::size_t offset,
               const std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    destination[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] std::uint32_t load_u32(const std::span<const std::byte> source,
                                     const std::size_t offset) noexcept {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    result |= std::to_integer<std::uint32_t>(source[offset + index])
              << (index * 8U);
  }
  return result;
}

[[nodiscard]] bool
equal_bytes(const std::span<const std::byte> left,
            const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size())
    return false;
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0;
}

template <std::size_t Size>
[[nodiscard]] bool starts_with(
    const std::span<const std::byte> bytes,
    const std::array<std::byte, Size> &prefix) noexcept {
  return bytes.size() >= prefix.size() &&
         equal_bytes(bytes.first(prefix.size()), prefix);
}

[[nodiscard]] Digest sha256(const std::span<const std::byte> input) {
  if (input.size() > std::numeric_limits<ULONG>::max()) {
    fail(CardError::invalid_configuration, "hash card journal bytes");
  }
  AlgorithmHandle algorithm;
  require_nt_success(::BCryptOpenAlgorithmProvider(
                         algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                     "open SHA-256 for card journal");
  Digest digest{};
  require_nt_success(
      ::BCryptHash(
          algorithm.get(), nullptr, 0,
          const_cast<PUCHAR>(reinterpret_cast<const UCHAR *>(input.data())),
          static_cast<ULONG>(input.size()),
          reinterpret_cast<PUCHAR>(digest.data()),
          static_cast<ULONG>(digest.size())),
      "hash card journal bytes");
  return digest;
}

[[nodiscard]] CommitId random_commit_id() {
  CommitId result{};
  require_nt_success(::BCryptGenRandom(nullptr,
                                       reinterpret_cast<PUCHAR>(result.data()),
                                       static_cast<ULONG>(result.size()),
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                     "generate card journal commit identifier");
  return result;
}

void fill_random(const std::span<std::byte> destination) {
  if (destination.size() > std::numeric_limits<ULONG>::max()) {
    fail(CardError::invalid_configuration, "generate card journal filler");
  }
  require_nt_success(
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(destination.data()),
                        static_cast<ULONG>(destination.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG),
      "generate card journal filler");
}

[[nodiscard]] CommitId distinct_commit_id(const CommitId &first,
                                          const CommitId &second) {
  for (;;) {
    CommitId candidate = random_commit_id();
    if (!equal_bytes(candidate, first) && !equal_bytes(candidate, second)) {
      return candidate;
    }
  }
}

void require_capacity(ICardRecordJournalStorage &storage) {
  if (storage.capacity() < kCardRecordJournalV1StorageSize) {
    fail(CardError::invalid_configuration,
         "card storage is too small for the record journal");
  }
}

[[nodiscard]] security::SecureBuffer
read_exact(ICardRecordJournalStorage &storage, const std::size_t offset,
           const std::size_t length) {
  security::SecureBuffer result = storage.read(offset, length);
  if (result.size() != length) {
    fail(CardError::device_failure,
         "card journal storage returned a short read");
  }
  return result;
}

[[nodiscard]] std::size_t slot_offset(const std::uint32_t slot) noexcept {
  return slot == 0 ? kCardRecordJournalV1Slot0Offset
                   : kCardRecordJournalV1Slot1Offset;
}

[[nodiscard]] std::size_t
descriptor_offset(const std::uint32_t descriptor) noexcept {
  return descriptor == 0 ? kCardRecordJournalV1Descriptor0Offset
                         : kCardRecordJournalV1Descriptor1Offset;
}

[[nodiscard]] DescriptorBytes
make_descriptor(const CommitId &commit_id, const CommitId &previous_id,
                const std::uint32_t slot,
                const std::span<const std::byte> record,
                const std::span<const std::byte> inactive) {
  DescriptorBytes descriptor{};
  std::ranges::copy(kDescriptorMagic, descriptor.begin());
  store_u32(descriptor, 8, kDescriptorVersion);
  store_u32(descriptor, 12, static_cast<std::uint32_t>(descriptor.size()));
  std::ranges::copy(commit_id, descriptor.begin() + kDescriptorCommitIdOffset);
  std::ranges::copy(previous_id,
                    descriptor.begin() + kDescriptorPreviousIdOffset);
  store_u32(descriptor, kDescriptorSlotOffset, slot);
  store_u32(descriptor, kDescriptorRecordLengthOffset,
            static_cast<std::uint32_t>(record.size()));
  const Digest record_digest = sha256(record);
  std::ranges::copy(record_digest,
                    descriptor.begin() + kDescriptorRecordDigestOffset);
  const Digest inactive_digest = sha256(inactive);
  std::ranges::copy(inactive_digest,
                    descriptor.begin() + kDescriptorInactiveDigestOffset);
  const Digest descriptor_digest =
      sha256(std::span(descriptor).first(kDescriptorPrefixSize));
  std::ranges::copy(descriptor_digest,
                    descriptor.begin() + kDescriptorDigestOffset);
  return descriptor;
}

[[nodiscard]] ParsedDescriptor
parse_descriptor(const std::span<const std::byte> descriptor,
                 const std::array<Digest, 2> &slot_digests) {
  if (descriptor.size() != kCardRecordJournalV1DescriptorSize ||
      !equal_bytes(descriptor.first(kDescriptorMagic.size()),
                   kDescriptorMagic) ||
      load_u32(descriptor, 8) != kDescriptorVersion ||
      load_u32(descriptor, 12) != descriptor.size() ||
      load_u32(descriptor, kDescriptorRecordLengthOffset) !=
          kCardRecordV1Size) {
    return {};
  }
  const std::uint32_t slot = load_u32(descriptor, kDescriptorSlotOffset);
  if (slot > 1)
    return {};

  const Digest expected_descriptor_digest =
      sha256(descriptor.first(kDescriptorPrefixSize));
  if (!equal_bytes(descriptor.subspan(kDescriptorDigestOffset, kDigestSize),
                   expected_descriptor_digest)) {
    return {};
  }

  CommitId commit_id{};
  CommitId previous_id{};
  std::ranges::copy(
      descriptor.subspan(kDescriptorCommitIdOffset, kCommitIdSize),
      commit_id.begin());
  std::ranges::copy(
      descriptor.subspan(kDescriptorPreviousIdOffset, kCommitIdSize),
      previous_id.begin());
  if (equal_bytes(commit_id, previous_id))
    return {};
  return {
      .valid = true,
      .record_matches = equal_bytes(
          descriptor.subspan(kDescriptorRecordDigestOffset, kDigestSize),
          slot_digests[slot]),
      .inactive_matches = equal_bytes(
          descriptor.subspan(kDescriptorInactiveDigestOffset, kDigestSize),
          slot_digests[slot ^ 1U]),
      .commit_id = commit_id,
      .previous_id = previous_id,
      .slot = slot,
  };
}

[[nodiscard]] LoadedJournal load_journal(ICardRecordJournalStorage &storage) {
  require_capacity(storage);
  std::array<security::SecureBuffer, 2> descriptors{
      read_exact(storage, kCardRecordJournalV1Descriptor0Offset,
                 kCardRecordJournalV1DescriptorSize),
      read_exact(storage, kCardRecordJournalV1Descriptor1Offset,
                 kCardRecordJournalV1DescriptorSize)};
  std::array<security::SecureBuffer, 2> slots{
      read_exact(storage, kCardRecordJournalV1Slot0Offset, kCardRecordV1Size),
      read_exact(storage, kCardRecordJournalV1Slot1Offset, kCardRecordV1Size)};
  const std::array<Digest, 2> slot_digests{sha256(slots[0].bytes()),
                                           sha256(slots[1].bytes())};
  const std::array<ParsedDescriptor, 2> parsed{
      parse_descriptor(descriptors[0].bytes(), slot_digests),
      parse_descriptor(descriptors[1].bytes(), slot_digests)};

  std::uint32_t selected = 0;
  if (!parsed[0].valid && !parsed[1].valid) {
    fail(CardError::device_failure,
         "card record journal has no recoverable commit");
  }
  if (!parsed[0].valid) {
    selected = 1;
  } else if (parsed[1].valid &&
             !equal_bytes(descriptors[0].bytes(), descriptors[1].bytes())) {
    const bool same_commit =
        equal_bytes(parsed[0].commit_id, parsed[1].commit_id) &&
        equal_bytes(parsed[0].previous_id, parsed[1].previous_id) &&
        parsed[0].slot == parsed[1].slot;
    if (same_commit) {
      if (parsed[0].inactive_matches == parsed[1].inactive_matches) {
        fail(CardError::device_failure,
             "card record journal has ambiguous mirrored metadata");
      }
      selected = parsed[1].inactive_matches ? 1U : 0U;
    } else {
      const bool zero_follows_one =
          equal_bytes(parsed[0].previous_id, parsed[1].commit_id);
      const bool one_follows_zero =
          equal_bytes(parsed[1].previous_id, parsed[0].commit_id);
      if (zero_follows_one == one_follows_zero) {
        fail(CardError::device_failure,
             "card record journal has an ambiguous commit chain");
      }
      if (parsed[0].slot == parsed[1].slot) {
        fail(CardError::device_failure,
             "card record journal commit chain reused one slot");
      }
      selected = one_follows_zero ? 1U : 0U;
    }
  }

  if (!parsed[selected].record_matches) {
    fail(CardError::device_failure,
         "active card record journal commit is damaged");
  }
  const std::uint32_t selected_slot = parsed[selected].slot;
  return {.record = std::move(slots[selected_slot]),
          .commit_id = parsed[selected].commit_id,
          .previous_id = parsed[selected].previous_id,
          .slot = selected_slot,
          .descriptor = selected,
          .mirrored =
              parsed[0].valid && parsed[1].valid &&
              equal_bytes(descriptors[0].bytes(), descriptors[1].bytes()) &&
              parsed[selected].inactive_matches};
}

void validate_logical_record(const std::span<const std::byte> record) {
  const CardRecordState state = inspect_card_record(record);
  if (state != CardRecordState::provisioned_uninitialized &&
      state != CardRecordState::initialized_v1) {
    fail(CardError::invalid_configuration,
         "card journal accepts only a complete v1 logical record");
  }
}

void write_verified(ICardRecordJournalStorage &storage,
                    const std::size_t offset,
                    const std::span<const std::byte> intended) {
  std::exception_ptr write_failure;
  try {
    storage.write(offset, intended);
  } catch (...) {
    write_failure = std::current_exception();
  }

  security::SecureBuffer observed;
  try {
    observed = read_exact(storage, offset, intended.size());
  } catch (...) {
    fail(CardError::device_failure,
         "card journal write outcome could not be verified");
  }
  if (equal_bytes(observed.bytes(), intended))
    return;
  if (write_failure)
    std::rethrow_exception(write_failure);
  fail(CardError::device_failure, "card journal writeback did not match");
}

} // namespace

security::SecureBuffer
read_card_record_journal_v1(ICardRecordJournalStorage &storage) {
  LoadedJournal journal = load_journal(storage);
  if (!journal.mirrored) {
    security::SecureBuffer filler(kCardRecordV1Size);
    fill_random(filler.bytes());
    write_verified(storage, slot_offset(journal.slot ^ 1U), filler.bytes());
    const DescriptorBytes normalized_descriptor =
        make_descriptor(journal.commit_id, journal.previous_id, journal.slot,
                        journal.record.bytes(), filler.bytes());
    write_verified(storage, descriptor_offset(journal.descriptor ^ 1U),
                   normalized_descriptor);
    write_verified(storage, descriptor_offset(journal.descriptor),
                   normalized_descriptor);

    LoadedJournal normalized = load_journal(storage);
    if (!normalized.mirrored ||
        !equal_bytes(normalized.commit_id, journal.commit_id) ||
        normalized.slot != journal.slot ||
        !equal_bytes(normalized.record.bytes(), journal.record.bytes())) {
      fail(CardError::device_failure,
           "card record journal recovery did not normalize storage");
    }
    return std::move(normalized.record);
  }
  return std::move(journal.record);
}

CardRecordJournalMediaState inspect_card_record_journal_media_v1(
    ICardRecordJournalStorage &storage) {
  require_capacity(storage);
  std::array<security::SecureBuffer, 2> descriptors{
      read_exact(storage, kCardRecordJournalV1Descriptor0Offset,
                 kCardRecordJournalV1DescriptorSize),
      read_exact(storage, kCardRecordJournalV1Descriptor1Offset,
                 kCardRecordJournalV1DescriptorSize)};
  std::array<security::SecureBuffer, 2> slots{
      read_exact(storage, kCardRecordJournalV1Slot0Offset,
                 kCardRecordV1Size),
      read_exact(storage, kCardRecordJournalV1Slot1Offset,
                 kCardRecordV1Size)};
  const std::array<Digest, 2> slot_digests{
      sha256(slots[0].bytes()), sha256(slots[1].bytes())};
  const std::array<ParsedDescriptor, 2> parsed{
      parse_descriptor(descriptors[0].bytes(), slot_digests),
      parse_descriptor(descriptors[1].bytes(), slot_digests)};

  const bool valid_descriptor = parsed[0].valid || parsed[1].valid;
  const bool descriptor_evidence =
      starts_with(descriptors[0].bytes(), kDescriptorPrefix) ||
      starts_with(descriptors[1].bytes(), kDescriptorPrefix);
  const bool provisioning_evidence =
      starts_with(slots[0].bytes(), kProvisioningPrefix) ||
      starts_with(slots[1].bytes(), kProvisioningPrefix);
  const bool complete_provisioning_marker =
      inspect_card_record(slots[0].bytes()) ==
          CardRecordState::provisioned_uninitialized ||
      inspect_card_record(slots[1].bytes()) ==
          CardRecordState::provisioned_uninitialized;
  const bool initialized_evidence =
      starts_with(slots[0].bytes(), kInitializedPrefix) ||
      starts_with(slots[1].bytes(), kInitializedPrefix);

  if (valid_descriptor || initialized_evidence) {
    return CardRecordJournalMediaState::product_evidence;
  }
  if (complete_provisioning_marker) {
    return CardRecordJournalMediaState::provisioning_interrupted;
  }
  if (descriptor_evidence || provisioning_evidence) {
    return CardRecordJournalMediaState::product_evidence;
  }
  return CardRecordJournalMediaState::no_product_evidence;
}

CardRecordState inspect_card_record_journal_logical_state_v1_read_only(
    ICardRecordJournalStorage &storage) {
  const LoadedJournal journal = load_journal(storage);
  return inspect_card_record(journal.record.bytes());
}

void provision_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    const std::span<const std::byte> provisioning_record) {
  require_capacity(storage);
  if (inspect_card_record(provisioning_record) !=
      CardRecordState::provisioned_uninitialized) {
    fail(CardError::invalid_configuration,
         "journal provisioning requires the exact uninitialized marker");
  }
  if (inspect_card_record_journal_media_v1(storage) ==
      CardRecordJournalMediaState::product_evidence) {
    fail(CardError::invalid_operation,
         "journal provisioning refuses existing product evidence");
  }

  const CommitId commit_id = random_commit_id();
  const CommitId previous_id = distinct_commit_id(commit_id, commit_id);
  std::array<std::byte, 1> slot_random{};
  fill_random(slot_random);
  const std::uint32_t initial_slot =
      std::to_integer<std::uint32_t>(slot_random.front()) & 1U;
  security::SecureBuffer filler(kCardRecordV1Size);
  fill_random(filler.bytes());
  write_verified(storage, slot_offset(initial_slot ^ 1U), filler.bytes());
  write_verified(storage, slot_offset(initial_slot), provisioning_record);
  const DescriptorBytes descriptor =
      make_descriptor(commit_id, previous_id, initial_slot, provisioning_record,
                      filler.bytes());
  write_verified(storage, kCardRecordJournalV1Descriptor0Offset, descriptor);
  write_verified(storage, kCardRecordJournalV1Descriptor1Offset, descriptor);
}

void bootstrap_uninitialized_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    const std::span<const std::byte> provisioning_record) {
  require_capacity(storage);
  if (inspect_card_record(provisioning_record) !=
      CardRecordState::provisioned_uninitialized) {
    fail(CardError::invalid_configuration,
         "journal bootstrap requires the exact uninitialized marker");
  }

  const CardRecordJournalMediaState state =
      inspect_card_record_journal_media_v1(storage);
  if (state == CardRecordJournalMediaState::product_evidence) {
    security::SecureBuffer existing =
        read_card_record_journal_v1(storage);
    if (equal_bytes(existing.bytes(), provisioning_record)) return;
    fail(CardError::invalid_operation,
         "journal bootstrap refuses an existing product record");
  }

  provision_card_record_journal_v1(storage, provisioning_record);
  security::SecureBuffer observed = read_card_record_journal_v1(storage);
  if (!equal_bytes(observed.bytes(), provisioning_record)) {
    fail(CardError::device_failure,
         "journal bootstrap did not publish the provisioning marker");
  }
}

void commit_card_record_journal_v1(
    ICardRecordJournalStorage &storage,
    const std::span<const std::byte> replacement_record) {
  validate_logical_record(replacement_record);
  LoadedJournal current = load_journal(storage);
  validate_logical_record(current.record.bytes());

  const std::uint32_t target_slot = current.slot ^ 1U;
  const std::uint32_t target_descriptor = current.descriptor ^ 1U;
  write_verified(storage, slot_offset(target_slot), replacement_record);
  const CommitId commit_id =
      distinct_commit_id(current.commit_id, current.previous_id);
  security::SecureBuffer filler(kCardRecordV1Size);
  fill_random(filler.bytes());
  const DescriptorBytes descriptor =
      make_descriptor(commit_id, current.commit_id, target_slot,
                      replacement_record, filler.bytes());
  write_verified(storage, descriptor_offset(target_descriptor), descriptor);

  // The new descriptor is now authoritative. Remove the prior logical record
  // before mirroring metadata so a stable card retains no readable update
  // history. If either cleanup step tears, the random commit link still selects
  // the already-verified new record and a retry can finish normalization.
  write_verified(storage, slot_offset(current.slot), filler.bytes());
  write_verified(storage, descriptor_offset(current.descriptor), descriptor);

  LoadedJournal committed = load_journal(storage);
  if (!equal_bytes(committed.commit_id, commit_id) ||
      committed.slot != target_slot ||
      !equal_bytes(committed.record.bytes(), replacement_record)) {
    fail(CardError::device_failure,
         "card record journal commit did not become active");
  }
}

} // namespace usb_vault::card
