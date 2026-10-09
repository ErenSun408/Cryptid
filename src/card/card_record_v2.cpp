#include "usb_vault/card/card_record_v2.h"

#include "card/card_record_crypto.h"
#include "card/card_record_v2_internal.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace usb_vault::card {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
    std::byte{'D'}, std::byte{'2'}, std::byte{}, std::byte{}};
constexpr std::uint32_t kMetadataVersion = 1;
constexpr std::size_t kMetadataPlaintextSize = 32;

static_assert(kCardRecordV2ContextOffset == kCardRecordV2HeaderSize);
static_assert(kCardRecordV2EnvelopeOffset ==
    kCardRecordV2ContextOffset + kCardRecordV2ContextSize);
static_assert(kCardRecordV2MetadataOffset == kCardRecordV2EnvelopeOffset +
    kCardRecordV2SlotCount * kCardRecordV2EnvelopeSize);
static_assert(kCardRecordV2Size ==
    kCardRecordV2MetadataOffset + kCardRecordV2MetadataSize);
static_assert(kCardRecordV2MetadataSize == 12 + kMetadataPlaintextSize + 16);
static_assert(kCardRecordV2ContextSize == kCardRecordV1HeaderSize);
static_assert(kCardRecordV2EnvelopeSize == kCardRecordV1EnvelopeSize);
static_assert(kCardRecordV2RootSecretSize == kCardRecordV1RootSecretSize);

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

void write_u32(const std::span<std::byte> bytes, const std::size_t offset,
    const std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

[[nodiscard]] bool valid_context(
    const std::span<const std::byte> context) noexcept {
  if (context.size() != kCardRecordV2ContextSize) return false;
  std::array<std::byte, kCardRecordV1Size> synthetic{};
  std::ranges::copy(context, synthetic.begin());
  return inspect_card_record(synthetic) == CardRecordState::initialized_v1;
}

[[nodiscard]] auto context_of(const std::span<const std::byte> record) {
  return record.subspan(kCardRecordV2ContextOffset, kCardRecordV2ContextSize);
}

[[nodiscard]] security::SecureBuffer frame_record(
    const std::span<const std::byte> context) {
  if (!valid_context(context)) {
    throw std::invalid_argument("invalid v2 authentication context");
  }
  security::SecureBuffer result(kCardRecordV2Size);
  detail::fill_card_random(result.bytes());
  std::ranges::fill(result.bytes().first(kCardRecordV2HeaderSize), std::byte{});
  std::ranges::copy(kMagic, result.bytes().begin());
  write_u32(result.bytes(), 8, kCardRecordV2Version);
  write_u32(result.bytes(), 12, static_cast<std::uint32_t>(kCardRecordV2Size));
  std::ranges::copy(context,
      result.bytes().begin() + kCardRecordV2ContextOffset);
  return result;
}

[[nodiscard]] std::uint32_t random_initial_slot() {
  // 255 accepted byte values divide equally between the three possible slots.
  // Do not bias the initial cursor or encode credential occupancy in it.
  security::SecureBuffer random(1);
  unsigned int value = 0;
  do {
    detail::fill_card_random(random.bytes());
    value = std::to_integer<unsigned int>(random.bytes()[0]);
  } while (value == 255U);
  random.clear();
  return value % 3U;
}

} // namespace

CardRecordV2State inspect_card_record_v2(
    const std::span<const std::byte> record) noexcept {
  if (record.size() < kMagic.size() ||
      !std::ranges::equal(record.first(kMagic.size()), kMagic)) {
    return CardRecordV2State::malformed;
  }
  if (record.size() < 12) return CardRecordV2State::malformed;
  if (read_u32(record, 8) != kCardRecordV2Version) {
    return CardRecordV2State::unsupported_version;
  }
  if (record.size() != kCardRecordV2Size ||
      read_u32(record, 12) != kCardRecordV2Size ||
      !std::ranges::all_of(record.subspan(16, 16),
          [](const std::byte value) { return value == std::byte{}; }) ||
      !valid_context(context_of(record))) {
    return CardRecordV2State::malformed;
  }
  return CardRecordV2State::initialized;
}

InitializedCardRecordV2 initialize_card_record_and_root_v2(
    security::SecureBuffer password_utf8) {
  auto context = detail::make_card_context_v1();
  security::SecureBuffer root(kCardRecordV2RootSecretSize);
  detail::fill_card_random(root.bytes());
  auto record = detail::build_initial_card_record_v2(context.bytes(), root,
      password_utf8, random_initial_slot());
  password_utf8.clear();
  return {.record = std::move(record), .root_secret = std::move(root)};
}

security::SecureBuffer unlock_card_record_v2(
    const std::span<const std::byte> record,
    security::SecureBuffer password_utf8) {
  auto result = detail::authenticate_card_record_v2(record,
      std::move(password_utf8));
  return std::move(result.root_secret);
}

security::SecureBuffer upgrade_card_record_v1_to_v2(
    const std::span<const std::byte> record,
    security::SecureBuffer password_utf8) {
  // The old authenticator validates shape and credentials; no attempt is made
  // to infer the state or root of an envelope this password cannot decrypt.
  auto root = unlock_card_record_v1(record, std::move(password_utf8));
  auto result = frame_record(record.first(kCardRecordV1HeaderSize));
  std::ranges::copy(record.subspan(kCardRecordV1HeaderSize),
      result.bytes().begin() + kCardRecordV2EnvelopeOffset);
  detail::seal_card_ring_metadata_v2(result.bytes(), root, 2);
  root.clear();
  return result;
}

security::SecureBuffer append_card_password_v2(
    const std::span<const std::byte> record,
    security::SecureBuffer existing_password_utf8,
    security::SecureBuffer new_password_utf8) {
  // Keep the original solely to compare against the known current credential;
  // the new input is not probed against any unknown credential slot.
  security::SecureBuffer authentication_password(existing_password_utf8.size());
  std::ranges::copy(existing_password_utf8.bytes(),
      authentication_password.bytes().begin());
  auto authenticated = detail::authenticate_card_record_v2(record,
      std::move(authentication_password));
  unsigned int equal = static_cast<unsigned int>(
      existing_password_utf8.size() == new_password_utf8.size());
  unsigned int difference = 0;
  if (equal != 0U) {
    for (std::size_t i = 0; i < existing_password_utf8.size(); ++i) {
      difference |= std::to_integer<unsigned int>(
          existing_password_utf8.bytes()[i] ^ new_password_utf8.bytes()[i]);
    }
    equal &= static_cast<unsigned int>(difference == 0U);
  }
  existing_password_utf8.clear();
  if (equal != 0U) {
    throw std::invalid_argument("new password must differ from current password");
  }
  security::SecureBuffer result(kCardRecordV2Size);
  std::ranges::copy(record, result.bytes().begin());
  detail::encrypt_card_envelope_v1(context_of(result.bytes()),
      static_cast<std::byte>(authenticated.next_slot), authenticated.root_secret,
      new_password_utf8, result.bytes().subspan(kCardRecordV2EnvelopeOffset +
          authenticated.next_slot * kCardRecordV2EnvelopeSize,
          kCardRecordV2EnvelopeSize));
  new_password_utf8.clear();
  detail::seal_card_ring_metadata_v2(result.bytes(), authenticated.root_secret,
      detail::next_card_slot_v2(authenticated.next_slot));
  authenticated.root_secret.clear();
  return result;
}

namespace detail {

std::uint32_t next_card_slot_v2(const std::uint32_t slot) {
  if (slot >= kCardRecordV2SlotCount) {
    throw std::invalid_argument("invalid card ring cursor");
  }
  return (slot + 2U) % 3U;
}

void seal_card_ring_metadata_v2(const std::span<std::byte> record,
    const security::SecureBuffer& root, const std::uint32_t next_slot) {
  if (inspect_card_record_v2(record) != CardRecordV2State::initialized ||
      next_slot >= kCardRecordV2SlotCount) {
    throw std::invalid_argument("invalid card ring metadata input");
  }
  auto key = derive_card_ring_metadata_key_v2(root, context_of(record));
  security::SecureBuffer plaintext(kMetadataPlaintextSize);
  fill_card_random(plaintext.bytes());
  write_u32(plaintext.bytes(), 0, kMetadataVersion);
  write_u32(plaintext.bytes(), 4, next_slot);
  seal_card_aead(key, plaintext.bytes(), record.first(kCardRecordV2MetadataOffset),
      record.subspan(kCardRecordV2MetadataOffset, kCardRecordV2MetadataSize));
  plaintext.clear();
  key.clear();
}

security::SecureBuffer build_initial_card_record_v2(
    const std::span<const std::byte> context,
    const security::SecureBuffer& root,
    const security::SecureBuffer& password_utf8,
    const std::uint32_t first_slot) {
  if (first_slot >= kCardRecordV2SlotCount) {
    throw std::invalid_argument("invalid initial card slot");
  }
  auto record = frame_record(context);
  encrypt_card_envelope_v1(context, static_cast<std::byte>(first_slot), root,
      password_utf8, record.bytes().subspan(kCardRecordV2EnvelopeOffset +
          first_slot * kCardRecordV2EnvelopeSize, kCardRecordV2EnvelopeSize));
  seal_card_ring_metadata_v2(record.bytes(), root, next_card_slot_v2(first_slot));
  return record;
}

AuthenticatedCardRecordV2 authenticate_card_record_v2(
    const std::span<const std::byte> record,
    security::SecureBuffer password_utf8) {
  if (inspect_card_record_v2(record) != CardRecordV2State::initialized) {
    throw std::invalid_argument("card record is not an initialized v2 record");
  }
  auto master = derive_card_master_v1(password_utf8, context_of(record));
  password_utf8.clear();
  std::array<CardEnvelopeResult, kCardRecordV2SlotCount> envelopes;
  for (std::size_t i = 0; i < envelopes.size(); ++i) {
    envelopes[i] = decrypt_card_envelope_v1(context_of(record),
        static_cast<std::byte>(i), master,
        record.subspan(kCardRecordV2EnvelopeOffset + i * kCardRecordV2EnvelopeSize,
            kCardRecordV2EnvelopeSize));
  }
  master.clear();

  // All slots and root comparisons are evaluated. OR of masked successful
  // roots selects the common value without branching on which slot matched;
  // pairwise disagreement causes rejection even if the metadata happens to open.
  security::SecureBuffer root(kCardRecordV2RootSecretSize);
  unsigned int successes = 0;
  unsigned int difference = 0;
  for (std::size_t i = 0; i < envelopes.size(); ++i) {
    const unsigned int mask =
        0U - static_cast<unsigned int>(envelopes[i].authenticated);
    successes += static_cast<unsigned int>(envelopes[i].authenticated);
    for (std::size_t j = 0; j < root.size(); ++j) {
      root.bytes()[j] |= static_cast<std::byte>(mask &
          std::to_integer<unsigned int>(envelopes[i].root_secret.bytes()[j]));
    }
    for (std::size_t other = i + 1; other < envelopes.size(); ++other) {
      const unsigned int pair_mask = mask &
          (0U - static_cast<unsigned int>(envelopes[other].authenticated));
      for (std::size_t j = 0; j < root.size(); ++j) {
        difference |= pair_mask & std::to_integer<unsigned int>(
            envelopes[i].root_secret.bytes()[j] ^
            envelopes[other].root_secret.bytes()[j]);
      }
    }
  }
  for (auto& envelope : envelopes) envelope.root_secret.clear();

  auto metadata_key = derive_card_ring_metadata_key_v2(root, context_of(record));
  auto metadata = open_card_aead(metadata_key,
      record.subspan(kCardRecordV2MetadataOffset, kCardRecordV2MetadataSize),
      record.first(kCardRecordV2MetadataOffset));
  metadata_key.clear();
  const auto version = read_u32(metadata.plaintext.bytes(), 0);
  const auto cursor = read_u32(metadata.plaintext.bytes(), 4);
  const bool accepted = (successes != 0U) & (difference == 0U) &
      metadata.authenticated & (version == kMetadataVersion) &
      (cursor < kCardRecordV2SlotCount);
  metadata.plaintext.clear();
  if (!accepted) {
    root.clear();
    throw CardCredentialRejected();
  }
  return {.root_secret = std::move(root), .next_slot = cursor};
}

} // namespace detail
} // namespace usb_vault::card
