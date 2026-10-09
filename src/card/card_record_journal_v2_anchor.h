#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace usb_vault::card::detail {

inline constexpr std::size_t kCardV2ExtensionBase = 736;
inline constexpr std::size_t kCardV2ExtensionSize = 1072;
inline constexpr std::size_t kCardV2StorageSize = 1808;
inline constexpr std::size_t kCardV2AnchorSize = 216;
inline constexpr std::size_t kCardV2AnchorTagOffset = 184;
inline constexpr std::array<std::byte, 8> kCardV2AnchorMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
    std::byte{'D'}, std::byte{'2'}, std::byte{}, std::byte{}};

[[nodiscard]] inline std::uint32_t card_journal_u32(
    const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8U * i);
  }
  return value;
}

inline void card_journal_store_u32(const std::span<std::byte> bytes,
    const std::size_t offset, const std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
  }
}

[[nodiscard]] inline bool is_card_record_v2_anchor(
    const std::span<const std::byte> bytes) noexcept {
  return bytes.size() == kCardV2AnchorSize &&
      std::ranges::equal(bytes.first(8), kCardV2AnchorMagic) &&
      card_journal_u32(bytes, 8) == 2 &&
      card_journal_u32(bytes, 12) == kCardV2AnchorSize &&
      card_journal_u32(bytes, 16) == kCardV2ExtensionBase &&
      card_journal_u32(bytes, 20) == kCardV2ExtensionSize &&
      card_journal_u32(bytes, 24) == 384 &&
      card_journal_u32(bytes, 28) == 1;
}

} // namespace usb_vault::card::detail
