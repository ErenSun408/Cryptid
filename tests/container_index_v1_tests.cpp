#include "usb_vault/application/container_index_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::fill_container_index_slot_v1;
using usb_vault::application::kContainerIndexEntryV1Version;
using usb_vault::application::kContainerIndexV1FileSize;
using usb_vault::application::kContainerIndexV1KeySize;
using usb_vault::application::kContainerIndexV1PathMaximumLength;
using usb_vault::application::kContainerIndexV1SlotCount;
using usb_vault::application::kContainerIndexV1SlotSize;
using usb_vault::application::seal_container_index_entry_v1;
using usb_vault::application::try_open_container_index_entry_v1;
using usb_vault::security::SecureBuffer;

// These numbers are the shape of the file on disk. Changing any of them
// changes the size of the file, which is the one property of it an observer
// can measure without a key, so they are pinned here deliberately. They are
// pinned at compile time rather than in a case: they are constants either
// way, and a build that drifts should not get as far as producing a runner.
static_assert(kContainerIndexV1SlotSize == 768, "slot size drifted");
static_assert(kContainerIndexV1SlotCount == 1024, "slot count drifted");
static_assert(kContainerIndexV1FileSize == 786432, "file size drifted");
static_assert(kContainerIndexV1KeySize == 32, "key size drifted");
static_assert(kContainerIndexV1PathMaximumLength == 259, "path limit drifted");
static_assert(kContainerIndexEntryV1Version == 1, "entry version drifted");

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition) {
    fail(message);
  }
}

using Key = std::array<std::byte, kContainerIndexV1KeySize>;

// Test keys stand in for whatever the caller derived. This unit never learns
// where a key came from, so a fixed pattern exercises it exactly as a real
// derived key would.
[[nodiscard]] Key test_key(const unsigned char seed) {
  Key key{};
  for (std::size_t index = 0; index < key.size(); ++index) {
    key[index] = static_cast<std::byte>(
        static_cast<unsigned char>(seed + (index * 7U)));
  }
  return key;
}

[[nodiscard]] std::vector<std::byte> copy_of(const SecureBuffer &buffer) {
  const auto bytes = buffer.bytes();
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

[[nodiscard]] std::wstring path_of_length(const std::size_t length) {
  std::wstring path = L"C:\\";
  while (path.size() < length) {
    path.push_back(static_cast<wchar_t>(L'a' + (path.size() % 26U)));
  }
  path.resize(length);
  return path;
}

template <typename Callable>
void expect_invalid_argument(Callable &&callable, const char *const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::invalid_argument &) {
    return;
  }
  fail(message);
}

void test_round_trip_returns_the_same_path() {
  const Key key = test_key(0x11);
  const std::wstring path = L"D:\\vaults\\holiday.hc";

  const SecureBuffer slot = seal_container_index_entry_v1(key, path);
  expect(slot.size() == kContainerIndexV1SlotSize,
         "a sealed entry is not exactly one slot");

  const std::optional<std::wstring> opened =
      try_open_container_index_entry_v1(key, slot.bytes());
  expect(opened.has_value(), "a slot did not open under its own key");
  expect(*opened == path, "the opened path differs from the sealed path");
}

void test_non_ascii_paths_survive_as_code_units() {
  const Key key = test_key(0x22);
  std::wstring path = L"E:\\\u5bb9\u5668\\\u65e5\u5e38.hc";
  // A path may legitimately carry a surrogate pair; the slot stores code
  // units, so it has to come back with the pair intact.
  path.push_back(static_cast<wchar_t>(0xD83D));
  path.push_back(static_cast<wchar_t>(0xDE00));

  const SecureBuffer slot = seal_container_index_entry_v1(key, path);
  const std::optional<std::wstring> opened =
      try_open_container_index_entry_v1(key, slot.bytes());
  expect(opened.has_value(), "a non-ASCII path did not open");
  expect(*opened == path, "a non-ASCII path did not survive the round trip");
}

void test_longest_and_shortest_paths_produce_the_same_shape() {
  const Key key = test_key(0x33);
  const std::wstring shortest = L"X";
  const std::wstring longest = path_of_length(kContainerIndexV1PathMaximumLength);
  expect(longest.size() == kContainerIndexV1PathMaximumLength,
         "the long path fixture is the wrong length");

  const SecureBuffer short_slot = seal_container_index_entry_v1(key, shortest);
  const SecureBuffer long_slot = seal_container_index_entry_v1(key, longest);
  expect(short_slot.size() == long_slot.size(),
         "path length leaked into the size of a slot");

  const std::optional<std::wstring> opened_short =
      try_open_container_index_entry_v1(key, short_slot.bytes());
  const std::optional<std::wstring> opened_long =
      try_open_container_index_entry_v1(key, long_slot.bytes());
  expect(opened_short.has_value() && *opened_short == shortest,
         "the shortest path did not survive the round trip");
  expect(opened_long.has_value() && *opened_long == longest,
         "the longest path did not survive the round trip");
}

void test_sealing_twice_never_repeats_a_slot() {
  const Key key = test_key(0x44);
  const std::wstring path = L"D:\\vaults\\same.hc";

  const SecureBuffer first = seal_container_index_entry_v1(key, path);
  const SecureBuffer second = seal_container_index_entry_v1(key, path);
  expect(!std::ranges::equal(first.bytes(), second.bytes()),
         "the same path sealed twice produced identical slots");

  const std::optional<std::wstring> opened =
      try_open_container_index_entry_v1(key, second.bytes());
  expect(opened.has_value() && *opened == path,
         "the second sealing of a path did not open");
}

void test_a_foreign_key_and_random_filler_answer_alike() {
  const Key owner = test_key(0x55);
  const Key stranger = test_key(0x56);
  const SecureBuffer slot =
      seal_container_index_entry_v1(owner, L"D:\\vaults\\private.hc");

  expect(!try_open_container_index_entry_v1(stranger, slot.bytes()).has_value(),
         "a slot opened under a key that did not seal it");

  std::array<std::byte, kContainerIndexV1SlotSize> filler{};
  fill_container_index_slot_v1(filler);
  expect(!try_open_container_index_entry_v1(owner, filler).has_value(),
         "random filler was read as an entry");
  expect(!try_open_container_index_entry_v1(stranger, filler).has_value(),
         "random filler was read as an entry by a stranger");

  // Two fillers drawn in a row have to differ, or a file full of them would
  // announce itself as a file full of them.
  std::array<std::byte, kContainerIndexV1SlotSize> other_filler{};
  fill_container_index_slot_v1(other_filler);
  expect(!std::ranges::equal(filler, other_filler),
         "two filler slots came out identical");
}

void test_every_tampered_byte_closes_the_slot() {
  const Key key = test_key(0x66);
  const SecureBuffer slot =
      seal_container_index_entry_v1(key, L"D:\\vaults\\intact.hc");

  // One flipped bit anywhere in the slot -- nonce, ciphertext or tag -- has to
  // close it. The sampled offsets cover one byte of each region.
  const std::array<std::size_t, 5> offsets{0, 11, 12, kContainerIndexV1SlotSize - 17,
                                           kContainerIndexV1SlotSize - 1};
  for (const std::size_t offset : offsets) {
    std::vector<std::byte> damaged = copy_of(slot);
    damaged[offset] ^= std::byte{0x01};
    if (try_open_container_index_entry_v1(key, damaged).has_value()) {
      fail("a tampered slot still opened");
    }
  }

  const std::optional<std::wstring> opened =
      try_open_container_index_entry_v1(key, slot.bytes());
  expect(opened.has_value(), "the untouched slot stopped opening");
}

void test_a_slot_carries_no_position() {
  // Nothing binds a slot to where it sits, so the store stays free to place an
  // entry in any spare slot and to shuffle the file. A slot lifted out of one
  // position and read from another has to open exactly the same.
  const Key key = test_key(0x77);
  const std::wstring path = L"D:\\vaults\\moved.hc";
  const SecureBuffer slot = seal_container_index_entry_v1(key, path);

  std::vector<std::byte> file(kContainerIndexV1FileSize);
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    fill_container_index_slot_v1(
        std::span(file).subspan(index * kContainerIndexV1SlotSize,
                                kContainerIndexV1SlotSize));
  }

  const std::array<std::size_t, 3> positions{0, 129,
                                             kContainerIndexV1SlotCount - 1};
  for (const std::size_t position : positions) {
    const std::span<std::byte> target = std::span(file).subspan(
        position * kContainerIndexV1SlotSize, kContainerIndexV1SlotSize);
    std::ranges::copy(slot.bytes(), target.begin());
    const std::optional<std::wstring> opened =
        try_open_container_index_entry_v1(key, target);
    expect(opened.has_value() && *opened == path,
           "a slot stopped opening after it moved");
    fill_container_index_slot_v1(target);
  }
}

void test_two_keys_each_open_only_their_own_entry() {
  // This is the shape a container with a hidden volume writes: one entry under
  // the key of each password that can open it, and neither password learns
  // that the other entry is there.
  const Key daily = test_key(0x88);
  const Key duress = test_key(0x99);
  const std::wstring daily_path = L"D:\\vaults\\daily.hc";
  const std::wstring duress_path = L"D:\\vaults\\duress.hc";

  const SecureBuffer daily_slot =
      seal_container_index_entry_v1(daily, daily_path);
  const SecureBuffer duress_slot =
      seal_container_index_entry_v1(duress, duress_path);

  const std::optional<std::wstring> daily_view =
      try_open_container_index_entry_v1(daily, daily_slot.bytes());
  expect(daily_view.has_value() && *daily_view == daily_path,
         "the daily key did not open its own entry");
  expect(!try_open_container_index_entry_v1(daily, duress_slot.bytes())
              .has_value(),
         "the daily key opened the other entry");

  const std::optional<std::wstring> duress_view =
      try_open_container_index_entry_v1(duress, duress_slot.bytes());
  expect(duress_view.has_value() && *duress_view == duress_path,
         "the duress key did not open its own entry");
  expect(!try_open_container_index_entry_v1(duress, daily_slot.bytes())
              .has_value(),
         "the duress key opened the other entry");
}

void test_invalid_inputs_are_rejected_at_the_boundary() {
  const Key key = test_key(0xAA);
  const std::array<std::byte, kContainerIndexV1KeySize - 1> short_key{};
  const std::array<std::byte, kContainerIndexV1SlotSize - 1> short_slot{};

  expect_invalid_argument(
      [&] { static_cast<void>(seal_container_index_entry_v1(short_key, L"D:\\a.hc")); },
      "a key of the wrong size was accepted for sealing");
  expect_invalid_argument(
      [&] { static_cast<void>(seal_container_index_entry_v1(key, L"")); },
      "an empty path was accepted");
  expect_invalid_argument(
      [&] {
        static_cast<void>(seal_container_index_entry_v1(
            key, path_of_length(kContainerIndexV1PathMaximumLength + 1)));
      },
      "an over-long path was accepted");
  expect_invalid_argument(
      [&] {
        const std::wstring embedded(L"D:\\a\0b.hc", 9);
        static_cast<void>(seal_container_index_entry_v1(key, embedded));
      },
      "a path with an embedded null was accepted");

  const SecureBuffer slot = seal_container_index_entry_v1(key, L"D:\\a.hc");
  expect_invalid_argument(
      [&] {
        static_cast<void>(
            try_open_container_index_entry_v1(short_key, slot.bytes()));
      },
      "a key of the wrong size was accepted for opening");
  expect_invalid_argument(
      [&] {
        static_cast<void>(try_open_container_index_entry_v1(key, short_slot));
      },
      "a slot of the wrong size was accepted for opening");
  expect_invalid_argument([&] { fill_container_index_slot_v1(std::span<std::byte>{}); },
                          "an empty span was accepted as a slot to fill");
}

// Everything below builds and reads a slot by hand, from the layout the format
// documents rather than from the code under test. A round trip alone would
// pass just as happily on a writer and a reader that agreed with each other
// and with nothing else; these say what the bytes are.
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kSealedSize =
    kContainerIndexV1SlotSize - kNonceSize - kTagSize;
constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kPathLengthOffset = 4;
constexpr std::size_t kPathOffset = 6;

void must(const NTSTATUS status, const char *const operation) {
  if (!BCRYPT_SUCCESS(status)) {
    fail(operation);
  }
}

[[nodiscard]] PUCHAR as_uchar(const std::span<const std::byte> bytes) noexcept {
  return const_cast<PUCHAR>(reinterpret_cast<const UCHAR *>(bytes.data()));
}

void put_u16(const std::span<std::byte> bytes, const std::size_t offset,
             const std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

[[nodiscard]] std::uint16_t take_u16(const std::span<const std::byte> bytes,
                                     const std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<unsigned int>(bytes[offset]) |
      (std::to_integer<unsigned int>(bytes[offset + 1]) << 8U));
}

void put_u32(const std::span<std::byte> bytes, const std::size_t offset,
             const std::uint32_t value) {
  put_u16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
  put_u16(bytes, offset + 2,
          static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

[[nodiscard]] std::uint32_t take_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) {
  return static_cast<std::uint32_t>(take_u16(bytes, offset)) |
         (static_cast<std::uint32_t>(take_u16(bytes, offset + 2)) << 16U);
}

// Runs one AES-256-GCM operation with no associated data, which is the whole
// of what a slot is. `input` and `output` are the same length; on decryption a
// rejected tag reports back instead of aborting.
[[nodiscard]] bool gcm(const std::span<const std::byte> key,
                       const std::span<const std::byte> nonce,
                       const std::span<std::byte> tag,
                       const std::span<const std::byte> input,
                       const std::span<std::byte> output, const bool sealing) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  must(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr,
                                     0),
       "open AES provider");
  must(::BCryptSetProperty(
           algorithm, BCRYPT_CHAINING_MODE,
           reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(
               BCRYPT_CHAIN_MODE_GCM)),
           static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0),
       "set AES-GCM mode");

  ULONG object_size = 0;
  ULONG written = 0;
  must(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                           reinterpret_cast<PUCHAR>(&object_size),
                           sizeof(object_size), &written, 0),
       "read CNG object length");
  std::vector<std::byte> object(object_size);
  BCRYPT_KEY_HANDLE key_handle = nullptr;
  must(::BCryptGenerateSymmetricKey(
           algorithm, &key_handle, reinterpret_cast<PUCHAR>(object.data()),
           object_size, as_uchar(key), static_cast<ULONG>(key.size()), 0),
       "generate AES-GCM key");

  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = as_uchar(nonce);
  authentication.cbNonce = static_cast<ULONG>(nonce.size());
  authentication.pbTag = reinterpret_cast<PUCHAR>(tag.data());
  authentication.cbTag = static_cast<ULONG>(tag.size());

  const NTSTATUS status =
      sealing ? ::BCryptEncrypt(key_handle, as_uchar(input),
                                static_cast<ULONG>(input.size()),
                                &authentication, nullptr, 0,
                                reinterpret_cast<PUCHAR>(output.data()),
                                static_cast<ULONG>(output.size()), &written, 0)
              : ::BCryptDecrypt(key_handle, as_uchar(input),
                                static_cast<ULONG>(input.size()),
                                &authentication, nullptr, 0,
                                reinterpret_cast<PUCHAR>(output.data()),
                                static_cast<ULONG>(output.size()), &written, 0);
  static_cast<void>(::BCryptDestroyKey(key_handle));
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));

  if (!sealing && status == static_cast<NTSTATUS>(0xC000A002UL)) {
    return false;
  }
  must(status, sealing ? "hand-seal a slot" : "hand-open a slot");
  expect(written == output.size(), "AES-GCM produced an unexpected length");
  return true;
}

[[nodiscard]] std::vector<std::byte>
hand_sealed_slot(const std::span<const std::byte> key,
                 const std::span<const std::byte> sealed_contents) {
  expect(sealed_contents.size() == kSealedSize,
         "the hand-built plaintext is the wrong size");
  std::vector<std::byte> slot(kContainerIndexV1SlotSize);
  for (std::size_t index = 0; index < kNonceSize; ++index) {
    slot[index] = static_cast<std::byte>(
        static_cast<unsigned char>(0xA0U + index));
  }
  const std::span<std::byte> bytes(slot);
  static_cast<void>(gcm(key, bytes.first(kNonceSize), bytes.last(kTagSize),
                        sealed_contents, bytes.subspan(kNonceSize, kSealedSize),
                        true));
  return slot;
}

[[nodiscard]] std::vector<std::byte>
hand_opened_contents(const std::span<const std::byte> key,
                     const std::span<const std::byte> slot) {
  expect(slot.size() == kContainerIndexV1SlotSize,
         "the slot handed to the reference reader is the wrong size");
  std::vector<std::byte> contents(kSealedSize);
  std::vector<std::byte> tag(slot.last(kTagSize).begin(),
                             slot.last(kTagSize).end());
  expect(gcm(key, slot.first(kNonceSize), tag,
             slot.subspan(kNonceSize, kSealedSize), contents, false),
         "a slot the writer produced was rejected by the reference reader");
  return contents;
}

[[nodiscard]] std::vector<std::byte>
hand_built_contents(const std::uint32_t version, const std::uint16_t length,
                    const std::wstring_view path) {
  // Padding is a recognizable byte rather than zero, so a reader that stopped
  // at a terminator instead of honouring the length field would be caught.
  std::vector<std::byte> contents(kSealedSize, std::byte{0xEE});
  const std::span<std::byte> bytes(contents);
  put_u32(bytes, kVersionOffset, version);
  put_u16(bytes, kPathLengthOffset, length);
  for (std::size_t index = 0; index < path.size(); ++index) {
    put_u16(bytes, kPathOffset + (index * 2),
            static_cast<std::uint16_t>(path[index]));
  }
  return contents;
}

void test_the_writer_lays_out_the_documented_bytes() {
  const Key key = test_key(0xBB);
  const std::wstring path = L"D:\\vaults\\layout.hc";
  const SecureBuffer slot = seal_container_index_entry_v1(key, path);
  const std::vector<std::byte> contents =
      hand_opened_contents(key, slot.bytes());

  expect(take_u32(contents, kVersionOffset) == kContainerIndexEntryV1Version,
         "the version is not a little-endian u32 at offset 0");
  expect(take_u16(contents, kPathLengthOffset) == path.size(),
         "the path length is not a little-endian u16 at offset 4");
  for (std::size_t index = 0; index < path.size(); ++index) {
    expect(take_u16(contents, kPathOffset + (index * 2)) ==
               static_cast<std::uint16_t>(path[index]),
           "the path is not UTF-16LE at offset 6");
  }
}

void test_the_reader_accepts_a_hand_built_entry() {
  const Key key = test_key(0xCC);
  const std::wstring path = L"F:\\vaults\\byhand.hc";
  const std::vector<std::byte> slot = hand_sealed_slot(
      key, hand_built_contents(kContainerIndexEntryV1Version,
                               static_cast<std::uint16_t>(path.size()), path));

  const std::optional<std::wstring> opened =
      try_open_container_index_entry_v1(key, slot);
  expect(opened.has_value(), "an entry built to the format did not open");
  expect(*opened == path,
         "an entry built to the format did not read back as its path");
}

void test_the_reader_closes_on_a_version_or_length_it_cannot_trust() {
  const Key key = test_key(0xDD);
  const std::wstring path = L"F:\\vaults\\byhand.hc";
  const auto length = static_cast<std::uint16_t>(path.size());

  const std::vector<std::byte> future =
      hand_sealed_slot(key, hand_built_contents(2, length, path));
  expect(!try_open_container_index_entry_v1(key, future).has_value(),
         "a later format version was read as a v1 entry");

  const std::vector<std::byte> empty = hand_sealed_slot(
      key, hand_built_contents(kContainerIndexEntryV1Version, 0, path));
  expect(!try_open_container_index_entry_v1(key, empty).has_value(),
         "an entry claiming no path was accepted");

  const std::vector<std::byte> overlong = hand_sealed_slot(
      key,
      hand_built_contents(
          kContainerIndexEntryV1Version,
          static_cast<std::uint16_t>(kContainerIndexV1PathMaximumLength + 1),
          path));
  expect(!try_open_container_index_entry_v1(key, overlong).has_value(),
         "an entry claiming a path past the limit was accepted");
}

} // namespace

int main() {
  test_round_trip_returns_the_same_path();
  test_non_ascii_paths_survive_as_code_units();
  test_longest_and_shortest_paths_produce_the_same_shape();
  test_sealing_twice_never_repeats_a_slot();
  test_a_foreign_key_and_random_filler_answer_alike();
  test_every_tampered_byte_closes_the_slot();
  test_a_slot_carries_no_position();
  test_two_keys_each_open_only_their_own_entry();
  test_invalid_inputs_are_rejected_at_the_boundary();
  test_the_writer_lays_out_the_documented_bytes();
  test_the_reader_accepts_a_hand_built_entry();
  test_the_reader_closes_on_a_version_or_length_it_cannot_trust();
  std::cout << "container_index_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
