#include "usb_vault/backup/export_package_v1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using usb_vault::backup::export_package_v1_size;
using usb_vault::backup::ExportPackageKeyV1;
using usb_vault::backup::ExportPackageReaderV1;
using usb_vault::backup::ExportPackageRejected;
using usb_vault::backup::ExportPackageV1Header;
using usb_vault::backup::ExportPackageV1Salt;
using usb_vault::backup::ExportPackageWriterV1;
using usb_vault::backup::generate_export_package_v1_salt;
using usb_vault::backup::kExportPackageV1ChunkSize;
using usb_vault::backup::kExportPackageV1SaltSize;
using usb_vault::backup::kExportPackageV1StreamHeaderSize;
using usb_vault::backup::kExportPackageV1TagSize;
using usb_vault::security::SecureBuffer;

using Bytes = std::vector<std::byte>;

constexpr std::size_t kSealed = kExportPackageV1ChunkSize + kExportPackageV1TagSize;

[[noreturn]] void fail(const char* const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char* const message) {
  if (!condition) fail(message);
}

template <typename Exception, typename Action>
void expect_throws(Action&& action, const char* const message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  } catch (...) {
    fail(message);
  }
  fail(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] Bytes pattern(const std::size_t size, const unsigned seed) {
  Bytes bytes(size);
  std::uint32_t state = seed * 2654435761U + 1U;
  for (auto& b : bytes) {
    state = state * 1664525U + 1013904223U;
    b = static_cast<std::byte>(state >> 24);
  }
  return bytes;
}

[[nodiscard]] Bytes build(const std::string_view password,
                          const ExportPackageV1Salt& salt,
                          const ExportPackageV1Header& header,
                          const Bytes& payload) {
  Bytes package;
  ExportPackageWriterV1 writer(
      ExportPackageKeyV1::derive(secure_text(password), salt), header,
      [&](const std::span<const std::byte> bytes) {
        package.insert(package.end(), bytes.begin(), bytes.end());
      });
  // Odd write sizes cross chunk boundaries at unaligned offsets.
  std::span<const std::byte> rest(payload);
  while (!rest.empty()) {
    const std::size_t take = std::min<std::size_t>(rest.size(), 7777);
    writer.write(rest.first(take));
    rest = rest.subspan(take);
  }
  writer.finish();
  return package;
}

[[nodiscard]] ExportPackageReaderV1 reader_for(const Bytes& package,
                                               const std::string_view password,
                                               std::size_t& cursor) {
  cursor = 0;
  return ExportPackageReaderV1(
      package.size(),
      [&package, &cursor](const std::span<std::byte> out) {
        if (out.size() > package.size() - cursor) {
          throw std::runtime_error("package source ran out");
        }
        std::copy_n(package.begin() + static_cast<std::ptrdiff_t>(cursor),
                    out.size(), out.begin());
        cursor += out.size();
      },
      [password](const ExportPackageV1Salt& salt) {
        return ExportPackageKeyV1::derive(secure_text(password), salt);
      });
}

struct Opened final {
  ExportPackageV1Header header;
  Bytes payload;
};

[[nodiscard]] Opened open_all(const Bytes& package,
                              const std::string_view password) {
  std::size_t cursor = 0;
  ExportPackageReaderV1 reader = reader_for(package, password, cursor);
  Opened result{reader.header(), Bytes(static_cast<std::size_t>(reader.remaining()))};
  std::span<std::byte> rest(result.payload);
  while (!rest.empty()) {
    const std::size_t take = std::min<std::size_t>(rest.size(), 5003);
    reader.read(rest.first(take));
    rest = rest.subspan(take);
  }
  expect(reader.remaining() == 0, "reader must report the stream consumed");
  expect(cursor == package.size(), "reader must consume the whole package");
  return result;
}

void expect_rejected(const Bytes& package, const std::string_view password,
                     const char* const message) {
  expect_throws<ExportPackageRejected>(
      [&] { static_cast<void>(open_all(package, password)); }, message);
}

[[nodiscard]] ExportPackageV1Header header_for(const std::size_t stream) {
  const std::uint64_t body = stream - kExportPackageV1StreamHeaderSize;
  const std::uint64_t manifest = std::min<std::uint64_t>(body, 100);
  return ExportPackageV1Header{.manifest_bytes = manifest,
                               .data_bytes = body - manifest,
                               .entry_count = 3};
}

void test_round_trips_across_chunk_boundaries() {
  const ExportPackageV1Salt salt = generate_export_package_v1_salt();
  const std::size_t streams[] = {
      kExportPackageV1StreamHeaderSize,
      kExportPackageV1StreamHeaderSize + 1,
      kExportPackageV1ChunkSize - 1,
      kExportPackageV1ChunkSize,
      kExportPackageV1ChunkSize + 1,
      2 * kExportPackageV1ChunkSize,
      2 * kExportPackageV1ChunkSize + 5,
  };
  unsigned seed = 1;
  for (const std::size_t stream : streams) {
    const ExportPackageV1Header header = header_for(stream);
    const Bytes payload = pattern(stream - kExportPackageV1StreamHeaderSize, seed++);
    const Bytes package = build("correct horse battery", salt, header, payload);
    expect(package.size() == export_package_v1_size(header),
           "package size must match the size formula");
    expect(std::equal(salt.begin(), salt.end(), package.begin()),
           "a package must start with its salt");
    const Opened opened = open_all(package, "correct horse battery");
    expect(opened.header == header, "the header must round-trip");
    expect(opened.payload == payload, "the payload must round-trip");
  }
}

void test_packages_differ_with_fresh_salts() {
  const ExportPackageV1Header header = header_for(kExportPackageV1StreamHeaderSize + 40);
  const Bytes payload = pattern(40, 9);
  const Bytes first = build("same password here", generate_export_package_v1_salt(), header, payload);
  const Bytes second = build("same password here", generate_export_package_v1_salt(), header, payload);
  expect(first.size() == second.size(), "same content gives the same size");
  std::size_t equal = 0;
  for (std::size_t i = 0; i < first.size(); ++i) equal += first[i] == second[i] ? 1 : 0;
  // Random bytes agree about once in 256 positions; a fixed field would not.
  expect(equal < first.size() / 16, "fresh salts must leave no fixed bytes");
}

void test_tampering_is_rejected() {
  const std::size_t stream = 2 * kExportPackageV1ChunkSize + 100;
  const ExportPackageV1Header header = header_for(stream);
  const Bytes payload = pattern(stream - kExportPackageV1StreamHeaderSize, 21);
  const Bytes package = build("tamper test password", generate_export_package_v1_salt(), header, payload);
  expect(package.size() == kExportPackageV1SaltSize + 2 * kSealed + 100 + kExportPackageV1TagSize,
         "three-chunk layout");

  expect_rejected(package, "tamper test passwore", "a wrong password must be rejected");

  Bytes salt_flip = package;
  salt_flip[3] ^= std::byte{1};
  expect_rejected(salt_flip, "tamper test password", "a changed salt must be rejected");

  Bytes middle_flip = package;
  middle_flip[kExportPackageV1SaltSize + kSealed + 4242] ^= std::byte{0x80};
  expect_rejected(middle_flip, "tamper test password", "a changed middle chunk must be rejected");

  Bytes tag_flip = package;
  tag_flip.back() ^= std::byte{1};
  expect_rejected(tag_flip, "tamper test password", "a changed final tag must be rejected");

  Bytes dropped_final(package.begin(), package.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + 2 * kSealed));
  expect_rejected(dropped_final, "tamper test password", "a dropped final chunk must be rejected");

  Bytes short_by_one(package.begin(), package.end() - 1);
  expect_rejected(short_by_one, "tamper test password", "a truncated package must be rejected");

  Bytes trailing = package;
  trailing.push_back(std::byte{0});
  expect_rejected(trailing, "tamper test password", "trailing bytes must be rejected");

  Bytes swapped = package;
  std::swap_ranges(swapped.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize),
                   swapped.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + kSealed),
                   swapped.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + kSealed));
  expect_rejected(swapped, "tamper test password", "reordered chunks must be rejected");

  const Bytes other = build("tamper test password", generate_export_package_v1_salt(), header, payload);
  Bytes spliced = package;
  std::copy_n(other.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + kSealed), kSealed,
              spliced.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + kSealed));
  expect_rejected(spliced, "tamper test password", "a chunk from another package must be rejected");

  expect_rejected(Bytes(kExportPackageV1SaltSize + kExportPackageV1TagSize, std::byte{7}),
                  "tamper test password", "a package too short for one byte must be rejected");
}

void test_chunk_binding_uses_whole_index_and_final_flag() {
  const ExportPackageKeyV1 key = ExportPackageKeyV1::derive(
      secure_text("chunk binding password"), generate_export_package_v1_salt());
  const Bytes plaintext = pattern(300, 33);
  Bytes sealed(plaintext.size() + kExportPackageV1TagSize);
  // Beyond 4 GiB of chunks: a nonce that kept only 32 bits would repeat here.
  const std::uint64_t index = (std::uint64_t{1} << 32) + 1;
  key.seal_chunk(index, false, plaintext, sealed);
  Bytes opened(plaintext.size());
  expect(key.open_chunk(index, false, sealed, opened) && opened == plaintext,
         "a chunk must open under its own index");
  expect(!key.open_chunk(1, false, sealed, opened), "the high index bits must be bound");
  expect(std::ranges::all_of(opened, [](const std::byte b) { return b == std::byte{0}; }),
         "a failed open must wipe the output");
  expect(!key.open_chunk(index, true, sealed, opened), "the final flag must be bound");
  Bytes wrong_size(plaintext.size() + 1);
  expect_throws<std::invalid_argument>(
      [&] { key.seal_chunk(index, false, plaintext, wrong_size); },
      "a mismatched sealed size must be refused");
}

void test_writer_and_reader_refuse_misuse() {
  const ExportPackageV1Salt salt = generate_export_package_v1_salt();
  const ExportPackageV1Header header = header_for(kExportPackageV1StreamHeaderSize + 10);
  const auto sink = [](std::span<const std::byte>) {};
  {
    ExportPackageWriterV1 writer(ExportPackageKeyV1::derive(secure_text("misuse password 1"), salt), header, sink);
    const Bytes too_much = pattern(11, 1);
    expect_throws<std::logic_error>([&] { writer.write(too_much); },
                                    "writing more than declared must be refused");
    const Bytes too_little = pattern(9, 1);
    writer.write(too_little);
    expect_throws<std::logic_error>([&] { writer.finish(); },
                                    "finishing short must be refused");
  }
  const Bytes package = build("misuse password 2", salt, header, pattern(10, 2));
  std::size_t cursor = 0;
  ExportPackageReaderV1 reader = reader_for(package, "misuse password 2", cursor);
  Bytes past_end(11);
  expect_throws<std::logic_error>([&] { reader.read(past_end); },
                                  "reading past the end must be refused");
  expect_throws<std::length_error>(
      [] {
        static_cast<void>(export_package_v1_size(
            {.manifest_bytes = ~std::uint64_t{0}, .data_bytes = 1, .entry_count = 0}));
      },
      "an overflowing header must be refused");
}

[[nodiscard]] Bytes from_hex(const std::string_view hex) {
  Bytes bytes(hex.size() / 2);
  const auto nibble = [](const char c) {
    return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
  };
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
  }
  return bytes;
}

// Computed independently with pyca/cryptography 50 (Argon2id with `ad`, HMAC,
// AESGCM) from the contract alone, so a drift in the KDF domains, nonce, AAD
// or header layout fails here even when a round trip still succeeds.
void test_known_answer_package() {
  ExportPackageV1Salt salt{};
  for (std::size_t i = 0; i < salt.size(); ++i) salt[i] = static_cast<std::byte>(i);
  const std::string_view text = "MANDATA!";
  const Bytes payload(reinterpret_cast<const std::byte*>(text.data()),
                      reinterpret_cast<const std::byte*>(text.data()) + text.size());
  const ExportPackageV1Header header{.manifest_bytes = 3, .data_bytes = 5, .entry_count = 1};
  const Bytes package = build("usb-vault export kat v1", salt, header, payload);
  expect(package == from_hex(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
      "724ccaaacdb4d00fd5d729763129f3d4951e0ed0f9972ea5c4a4c23d234aa60b"
      "ed2c731879e4702ce27d5d9fad2d7f7e5583a2c69a5a7321b66d6d07d596a365"
      "f86fc491bbbeac8f9ed1b5e626e3242048e730957ab733c4"),
         "the package must match the independent known answer");
}

void test_authenticated_header_must_match_layout() {
  const ExportPackageKeyV1 key = ExportPackageKeyV1::derive(
      secure_text("layout password"), generate_export_package_v1_salt());
  // A genuinely sealed single chunk whose header claims one byte more data
  // than the chunk holds: every tag verifies, only the sizes disagree.
  Bytes stream(kExportPackageV1StreamHeaderSize + 4);
  const std::string_view marker = "UVEXPKG1";
  std::copy_n(reinterpret_cast<const std::byte*>(marker.data()), marker.size(), stream.begin());
  stream[8] = std::byte{1};
  stream[24] = std::byte{5};
  Bytes package(key.salt().begin(), key.salt().end());
  Bytes sealed(stream.size() + kExportPackageV1TagSize);
  key.seal_chunk(0, true, stream, sealed);
  package.insert(package.end(), sealed.begin(), sealed.end());
  expect_rejected(package, "layout password", "a header disagreeing with the layout must be rejected");

  stream[24] = std::byte{4};
  key.seal_chunk(0, true, stream, sealed);
  std::copy(sealed.begin(), sealed.end(), package.begin() + kExportPackageV1SaltSize);
  expect(open_all(package, "layout password").payload.size() == 4,
         "the same chunk with a matching header must open");
}

void test_remainder_too_short_for_a_chunk_is_rejected() {
  const ExportPackageV1Header header = header_for(kExportPackageV1ChunkSize + 1);
  const Bytes package = build("remainder password", generate_export_package_v1_salt(), header,
                              pattern(kExportPackageV1ChunkSize + 1 - kExportPackageV1StreamHeaderSize, 5));
  for (const std::size_t remainder : {std::size_t{1}, kExportPackageV1TagSize}) {
    Bytes cut(package.begin(), package.begin() + static_cast<std::ptrdiff_t>(kExportPackageV1SaltSize + kSealed + remainder));
    expect_rejected(cut, "remainder password", "a final chunk without plaintext must be rejected");
  }
}

}  // namespace

int main() {
  test_known_answer_package();
  test_authenticated_header_must_match_layout();
  test_remainder_too_short_for_a_chunk_is_rejected();
  test_round_trips_across_chunk_boundaries();
  test_packages_differ_with_fresh_salts();
  test_tampering_is_rejected();
  test_chunk_binding_uses_whole_index_and_final_flag();
  test_writer_and_reader_refuse_misuse();
  std::cout << "export_package_v1_tests passed\n";
  return EXIT_SUCCESS;
}
