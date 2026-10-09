#include "card/card_record_journal_v2_reset.h"
#include "card/card_record_journal_v1_internal.h"
#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v2.h"

#include <algorithm>
#include <array>
#include <exception>
#include <system_error>

namespace usb_vault::card {
namespace {
struct Step { std::size_t offset, length; bool clear; };
constexpr std::array<Step, 10> kSteps{{
    {0, 152, true}, {152, 152, true}, {304, 216, false}, {520, 216, false},
    {736, 152, false}, {888, 152, false}, {1040, 384, false}, {1424, 384, false},
    {0, 152, false}, {152, 152, false}}};
[[noreturn]] void fail(CardError error, const char* message) {
  throw std::system_error(make_error_code(error), message);
}
security::SecureBuffer copy(std::span<const std::byte> bytes) {
  security::SecureBuffer result(bytes.size());
  std::ranges::copy(bytes, result.bytes().begin());
  return result;
}
bool equal(std::span<const std::byte> a, std::span<const std::byte> b) {
  return std::ranges::equal(a, b);
}
security::SecureBuffer read_reserved(ICardRecordJournalStorage& storage) {
  if (storage.capacity() < 1808) fail(CardError::invalid_configuration, "reset capacity too small");
  auto result = storage.read(0, 1808);
  if (result.size() != 1808) fail(CardError::device_failure, "short reset snapshot");
  return result;
}
security::SecureBuffer replacement(const Step step, std::span<const std::byte> target) {
  return step.clear ? security::SecureBuffer(step.length) : copy(target.subspan(step.offset, step.length));
}
// Recognize only an exact sequence prefix and at most one contiguous short write.
bool torn_step(std::span<const std::byte> observed, std::span<const std::byte> before,
    const Step step, std::span<const std::byte> after) {
  if (!equal(observed.first(step.offset), before.first(step.offset)) ||
      !equal(observed.subspan(step.offset + step.length), before.subspan(step.offset + step.length))) return false;
  const auto actual = observed.subspan(step.offset, step.length);
  std::size_t prefix = 0;
  while (prefix < step.length && actual[prefix] == after[prefix]) ++prefix;
  return equal(actual.subspan(prefix), before.subspan(step.offset + prefix, step.length - prefix));
}
bool known_reset_source(std::span<const std::byte> record) {
  if (inspect_card_record(record) == CardRecordState::provisioned_uninitialized) return true;
  // An interrupted marker write can retain an old anchor tail. This is only
  // evidence for a freshly confirmed destructive reset, never initialization.
  return record.size() == 216 && equal(record.first(8), detail::kCardV2AnchorMagic) &&
      detail::card_journal_u32(record, 8) == 2 && detail::card_journal_u32(record, 12) == 216 &&
      detail::card_journal_u32(record, 16) == 736 && detail::card_journal_u32(record, 20) == 1072 &&
      detail::card_journal_u32(record, 24) == 384 &&
      (detail::card_journal_u32(record, 28) == 1 || detail::card_journal_u32(record, 28) == 2);
}
} // namespace

PreparedCardJournalV2Reset prepare_card_journal_v2_reset(ICardRecordJournalStorage& storage,
    std::span<const std::byte> expected) {
  auto observed = read_reserved(storage);
  if (expected.size() != 1808 || !equal(observed.bytes(), expected)) {
    fail(CardError::invalid_operation, "reset snapshot changed");
  }
  bool legacy_initialized = false;
  try {
    legacy_initialized = inspect_card_record(detail::snapshot_legacy_card_journal(storage).record.bytes()) ==
        CardRecordState::initialized_v1;
  } catch (const std::system_error& error) {
    // Descriptor revocation may leave no selectable commit. Communication and
    // configuration errors still close preparation rather than becoming residue.
    if (error.code() != make_error_code(CardError::device_failure)) throw;
  }
  if (legacy_initialized) fail(CardError::invalid_operation, "v2 reset refuses active legacy initialized record");
  // Reset intentionally revokes descriptors first. After process loss, a new
  // native confirmation of this complete snapshot can discard known v2 residue.
  // Known footprints establish eligibility, not implicit destructive authority.
  bool known = false;
  for (const auto offset : {304U, 520U}) {
    const auto record = observed.bytes().subspan(offset, 216);
    if (equal(record.first(8), detail::kCardV2AnchorMagic)) {
      const auto version = detail::card_journal_u32(record, 8);
      if (version != 1 && (version != 2 || !known_reset_source(record))) {
        fail(CardError::invalid_operation, "reset refuses future legacy-region record");
      }
    }
    known = known || known_reset_source(record);
  }
  if (!known) fail(CardError::invalid_operation, "reset requires known provisioning or v2 footprint");
  for (const auto offset : {0U, 152U, 736U, 888U}) {
    const auto descriptor = observed.bytes().subspan(offset, 152);
    constexpr std::array<std::byte, 8> magic{std::byte{'U'}, std::byte{'V'}, std::byte{'J'},
        std::byte{'R'}, std::byte{'N'}, std::byte{'2'}, std::byte{}, std::byte{}};
    if (equal(descriptor.first(8), magic) && detail::card_journal_u32(descriptor, 8) != (offset < 736 ? 1U : 2U)) {
      fail(CardError::invalid_operation, "reset refuses future extension descriptor");
    }
  }
  for (const auto offset : {1040U, 1424U}) {
    if (inspect_card_record_v2(observed.bytes().subspan(offset, 384)) == CardRecordV2State::unsupported_version) {
      fail(CardError::invalid_operation, "reset refuses future logical record");
    }
  }
  if (!equal(read_reserved(storage).bytes(), observed.bytes())) {
    fail(CardError::invalid_operation, "reset source changed during preparation");
  }
  PreparedCardJournalV2Reset result;
  result.source_ = std::move(observed);
  result.target_ = security::SecureBuffer(1808);
  auto journal = detail::make_v2_provisioning_journal_image();
  std::ranges::copy(journal.bytes(), result.target_.bytes().begin());
  return result;
}

void reset_card_journal_v2(ICardRecordJournalStorage& storage,
    const PreparedCardJournalV2Reset& prepared) {
  if (prepared.source_.size() != 1808 || prepared.target_.size() != 1808) {
    fail(CardError::invalid_configuration, "reset preparation was moved");
  }
  auto observed = read_reserved(storage);
  if (equal(observed.bytes(), prepared.target_.bytes())) return;
  auto before = copy(prepared.source_.bytes());
  std::size_t resume = kSteps.size();
  for (std::size_t index = 0; index < kSteps.size(); ++index) {
    const auto step = kSteps[index];
    auto block = replacement(step, prepared.target_.bytes());
    if (torn_step(observed.bytes(), before.bytes(), step, block.bytes())) resume = index;
    std::ranges::copy(block.bytes(), before.bytes().begin() + step.offset);
  }
  if (resume == kSteps.size()) fail(CardError::invalid_operation, "reset state changed outside its write sequence");
  for (std::size_t index = resume; index < kSteps.size(); ++index) {
    const auto step = kSteps[index];
    auto block = replacement(step, prepared.target_.bytes());
    auto expected = copy(observed.bytes());
    std::ranges::copy(block.bytes(), expected.bytes().begin() + step.offset);
    if (equal(observed.bytes(), expected.bytes())) continue;
    if (!equal(read_reserved(storage).bytes(), observed.bytes())) {
      fail(CardError::invalid_operation, "reset changed before write");
    }
    std::exception_ptr reported;
    try { storage.write(step.offset, block.bytes()); }
    catch (...) { reported = std::current_exception(); }
    observed = read_reserved(storage);
    if (!equal(observed.bytes(), expected.bytes())) {
      if (reported) std::rethrow_exception(reported);
      fail(CardError::device_failure, "reset complete postimage mismatch");
    }
  }
  if (!equal(read_reserved(storage).bytes(), prepared.target_.bytes())) {
    fail(CardError::device_failure, "reset final image mismatch");
  }
}
} // namespace usb_vault::card
