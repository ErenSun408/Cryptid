#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace usb_vault::card {
namespace {

constexpr std::array<std::byte, 8> kInitializedMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
    std::byte{'D'}, std::byte{'2'}, std::byte{0},   std::byte{0}};
constexpr std::array<std::byte, 8> kProvisioningMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'B'}, std::byte{'L'},
    std::byte{'N'}, std::byte{'K'}, std::byte{'2'}, std::byte{0}};
constexpr std::uint32_t kCardMemoryKib = 512U * 1024U;
constexpr std::uint32_t kCardIterations = 2;
constexpr std::uint32_t kCardParallelism = 1;
constexpr std::uint32_t kArgon2Version13 = 0x13;

[[nodiscard]] std::uint32_t read_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool exact_provisioning_marker(
    const std::span<const std::byte> record) noexcept {
  return record.size() == kCardRecordV1Size &&
         std::ranges::equal(record.first(kProvisioningMagic.size()),
                            kProvisioningMagic) &&
         std::ranges::all_of(
             record.subspan(kProvisioningMagic.size()),
             [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] bool valid_v1_header(
    const std::span<const std::byte> record) noexcept {
  return record.size() == kCardRecordV1Size &&
         std::ranges::equal(record.first(kInitializedMagic.size()),
                            kInitializedMagic) &&
         read_u32(record, 8) == kCardRecordV1Version &&
         read_u32(record, 12) == kCardRecordV1Size &&
         read_u32(record, 48) == kCardMemoryKib &&
         read_u32(record, 52) == kCardIterations &&
         read_u32(record, 56) == kCardParallelism &&
         read_u32(record, 60) == kArgon2Version13;
}

} // namespace

CardRecordState
inspect_card_record(const std::span<const std::byte> record) noexcept {
  if (record.size() == kLegacyP1CardBlobSize) {
    return CardRecordState::legacy_p1_blob;
  }
  if (exact_provisioning_marker(record)) {
    return CardRecordState::provisioned_uninitialized;
  }
  if (valid_v1_header(record)) {
    return CardRecordState::initialized_v1;
  }
  if (record.size() >= kInitializedMagic.size() &&
      std::ranges::equal(record.first(kInitializedMagic.size()),
                         kInitializedMagic)) {
    if (record.size() >= 12 && read_u32(record, 8) != kCardRecordV1Version) {
      return CardRecordState::unsupported_version;
    }
    return CardRecordState::malformed;
  }
  return CardRecordState::malformed;
}

} // namespace usb_vault::card
