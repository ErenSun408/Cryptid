#include "usb_vault/backup/export_manifest_v1.h"
#include "usb_vault/backup/export_package_source.h"

#include "volume_test_support.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using usb_vault::backup::collect_export_manifest_v1;
using usb_vault::backup::decode_export_manifest_v1;
using usb_vault::backup::encode_export_manifest_v1;
using usb_vault::backup::export_package_v1_header;
using usb_vault::backup::ExportEntryKind;
using usb_vault::backup::ExportManifest;
using usb_vault::backup::ExportManifestEntry;
using usb_vault::backup::ExportPackageKeyV1;
using usb_vault::backup::ExportPackageReaderV1;
using usb_vault::backup::ExportPackageRejected;
using usb_vault::backup::ExportPackageV1Header;
using usb_vault::backup::ExportPackageV1Salt;
using usb_vault::backup::generate_export_package_v1_salt;
using usb_vault::backup::kExportManifestV1MaximumEntries;
using usb_vault::backup::kExportPackageV1ChunkSize;
using usb_vault::backup::kExportPackageV1SaltSize;
using usb_vault::backup::kExportPackageV1StreamHeaderSize;
using usb_vault::backup::kExportPackageV1TagSize;
using usb_vault::backup::read_export_manifest_v1;
using usb_vault::backup::validate_export_manifest_v1;
using usb_vault::backup::verify_export_package_v1;
using usb_vault::backup::write_export_package_v1;
using usb_vault::security::SecureBuffer;
using usb_vault_test::TemporaryDirectory;

using Bytes = std::vector<std::byte>;

constexpr std::string_view kPassword = "manifest test password";

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const std::string& message) {
  if (!condition) fail(message);
}

template <typename Exception, typename Action>
void expect_throws(Action&& action, const std::string& message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  } catch (const std::exception& error) {
    fail(message + " (threw " + error.what() + ")");
  }
  fail(message + " (did not throw)");
}

void expect_cancelled(const auto& action, const std::string& message) {
  try {
    action();
  } catch (const std::system_error& error) {
    expect(error.code().value() == ERROR_CANCELLED, message);
    return;
  }
  fail(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] ExportManifestEntry directory(std::wstring path) {
  return {.kind = ExportEntryKind::directory, .relative_path = std::move(path)};
}

[[nodiscard]] ExportManifestEntry file(std::wstring path, const std::uint64_t size) {
  return {.kind = ExportEntryKind::file, .relative_path = std::move(path), .size = size,
          .last_write_time = 133000000000000000ULL};
}

[[nodiscard]] ExportManifest manifest_of(std::vector<ExportManifestEntry> entries) {
  ExportManifest manifest{.entries = std::move(entries)};
  for (const auto& entry : manifest.entries) manifest.data_bytes += entry.size;
  return manifest;
}

[[nodiscard]] ExportManifest sample() {
  return manifest_of({directory(L"Docs"), file(L"Docs\\a.txt", 5), directory(L"Docs\\sub"),
                      file(L"Docs\\sub\\b.bin", 7), directory(L"Docs\\empty")});
}

[[nodiscard]] Bytes encoded_bytes(const ExportManifest& manifest) {
  const auto wiped = encode_export_manifest_v1(manifest);
  return Bytes(wiped.bytes().begin(), wiped.bytes().end());
}

void test_valid_manifests_pass_and_round_trip() {
  const ExportManifest manifest = sample();
  validate_export_manifest_v1(manifest);
  validate_export_manifest_v1(manifest_of({file(L"report.pdf", 10)}));
  validate_export_manifest_v1(manifest_of({file(L"报告.txt", 0)}));
  const Bytes encoded = encoded_bytes(manifest);
  const ExportManifest decoded =
      decode_export_manifest_v1(encoded, export_package_v1_header(manifest, encoded.size()));
  expect(decoded.entries == manifest.entries && decoded.data_bytes == manifest.data_bytes,
         "a manifest must round-trip");
}

void test_invalid_manifests_are_rejected() {
  const auto rejected = [](const ExportManifest& manifest, const std::string& why) {
    expect_throws<ExportPackageRejected>([&] { validate_export_manifest_v1(manifest); }, why);
    expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(encode_export_manifest_v1(manifest)); }, why + " (encode)");
  };
  rejected(manifest_of({}), "an empty manifest");
  rejected(manifest_of({file(L"a", 1), file(L"b", 1)}), "two top-level entries");
  rejected(manifest_of({file(L"a", 1), file(L"a\\b", 1)}), "an entry under a file");
  rejected(manifest_of({directory(L"a\\b")}), "a nested first entry");
  rejected(manifest_of({directory(L"a"), file(L"a\\x\\y", 1), directory(L"a\\x")}),
           "a child before its parent");
  rejected(manifest_of({directory(L"a"), file(L"a\\x", 1), file(L"a\\X", 1)}),
           "names equal ignoring case");
  rejected(manifest_of({directory(L"a"), file(L"b\\x", 1)}), "a missing parent");
  ExportManifest sized_directory = manifest_of({directory(L"a")});
  sized_directory.entries[0].size = 1;
  sized_directory.data_bytes = 1;
  rejected(sized_directory, "a directory with a size");
  ExportManifest wrong_total = sample();
  ++wrong_total.data_bytes;
  rejected(wrong_total, "a wrong data total");
  ExportManifest overflow = manifest_of({directory(L"a"), file(L"a\\x", ~std::uint64_t{0}), file(L"a\\y", 2)});
  overflow.data_bytes = 1;
  rejected(overflow, "overflowing sizes");
  ExportManifest bad_kind = manifest_of({file(L"a", 0)});
  bad_kind.entries[0].kind = static_cast<ExportEntryKind>(3);
  rejected(bad_kind, "an unknown kind");

  const std::wstring bad_names[] = {
      L"..", L".", L"a:b", L"a/b", L"a.", L"a ", L" a", L"CON", L"con.txt", L"COM1",
      L"LPT9.log", L"COM¹", L"CONIN$", L"conout$.txt", L"a*b", L"a?b", L"a<b",
      L"a\"b", L"a|b", L"NUL .txt", L"CON  .log", L"COM1 .x", L"com0", L"aux. .md", std::wstring(L"a") + L'\x01', L"C:", L"\\a", std::wstring(256, L'n')};
  for (const std::wstring& name : bad_names) {
    rejected(manifest_of({file(name, 0)}), "a bad top-level name");
    rejected(manifest_of({directory(L"top"), file(L"top\\" + name, 0)}), "a bad nested name");
  }
  rejected(manifest_of({directory(L"top"), file(L"top\\\\x", 0)}), "an empty component");
  rejected(manifest_of({directory(L"top"), directory(L"top\\")}), "a trailing separator");

  // Valid entry by entry, but too large once encoded: refused before the
  // buffer for it is allocated.
  ExportManifest oversized = manifest_of({directory(L"top")});
  for (std::size_t i = 0; i < 130'000; ++i) {
    std::wstring name = std::to_wstring(i);
    name.resize(250, L'n');
    oversized.entries.push_back(file(L"top\\" + name, 0));
  }
  validate_export_manifest_v1(oversized);
  expect_throws<std::invalid_argument>(
      [&] { static_cast<void>(encode_export_manifest_v1(oversized)); },
      "an encoded manifest over the size limit");
}

void test_malformed_manifest_bytes_are_rejected() {
  const ExportManifest manifest = sample();
  const Bytes encoded = encoded_bytes(manifest);
  const ExportPackageV1Header header = export_package_v1_header(manifest, encoded.size());
  const auto rejected = [](const Bytes& bytes, const ExportPackageV1Header& declared,
                           const std::string& why) {
    expect_throws<ExportPackageRejected>(
        [&] { static_cast<void>(decode_export_manifest_v1(bytes, declared)); }, why);
  };
  rejected(Bytes(encoded.begin(), encoded.end() - 1), {.manifest_bytes = encoded.size() - 1,
           .data_bytes = header.data_bytes, .entry_count = header.entry_count}, "truncated bytes");
  Bytes trailing = encoded;
  trailing.push_back(std::byte{0});
  rejected(trailing, {.manifest_bytes = trailing.size(), .data_bytes = header.data_bytes,
           .entry_count = header.entry_count}, "trailing bytes");
  rejected(encoded, {.manifest_bytes = encoded.size() + 1, .data_bytes = header.data_bytes,
           .entry_count = header.entry_count}, "a declared size that disagrees");
  rejected(encoded, {.manifest_bytes = encoded.size(), .data_bytes = header.data_bytes,
           .entry_count = header.entry_count - 1}, "fewer entries than encoded");
  rejected(encoded, {.manifest_bytes = encoded.size(), .data_bytes = header.data_bytes,
           .entry_count = header.entry_count + 1}, "more entries than encoded");
  rejected(encoded, {.manifest_bytes = encoded.size(), .data_bytes = header.data_bytes + 1,
           .entry_count = header.entry_count}, "a data total that disagrees");
  Bytes zero_length = encoded;
  zero_length[1] = std::byte{0};
  zero_length[2] = std::byte{0};
  rejected(zero_length, header, "an empty path");
}

[[nodiscard]] Bytes pattern(const std::size_t size) {
  Bytes bytes(size);
  for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::byte>((i * 131 + 7) & 0xFF);
  return bytes;
}

void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!output) throw std::runtime_error("write a fixture file");
}

[[nodiscard]] Bytes package_from(const std::wstring& source, const ExportManifest& manifest,
                                 const std::function<bool()>& cancelled = {}) {
  Bytes package;
  write_export_package_v1(
      source, manifest,
      ExportPackageKeyV1::derive(secure_text(kPassword), generate_export_package_v1_salt()),
      [&](const std::span<const std::byte> bytes) { package.insert(package.end(), bytes.begin(), bytes.end()); },
      cancelled);
  return package;
}

struct PackageSource final {
  const Bytes& package;
  std::size_t cursor{};
};

[[nodiscard]] ExportPackageReaderV1 open_package(PackageSource& source) {
  return ExportPackageReaderV1(
      source.package.size(),
      [&source](const std::span<std::byte> out) {
        if (out.size() > source.package.size() - source.cursor) throw std::runtime_error("ran out");
        std::copy_n(source.package.begin() + static_cast<std::ptrdiff_t>(source.cursor), out.size(), out.begin());
        source.cursor += out.size();
      },
      [](const ExportPackageV1Salt& salt) { return ExportPackageKeyV1::derive(secure_text(kPassword), salt); });
}

void test_directory_tree_exports_and_verifies() {
  const TemporaryDirectory temp;
  const auto docs = temp.root() / L"Docs";
  usb_vault_test::make_directory(docs / L"sub");
  usb_vault_test::make_directory(docs / L"empty");
  const Bytes small = pattern(5);
  const Bytes large = pattern(kExportPackageV1ChunkSize + kExportPackageV1ChunkSize / 2);
  write_bytes(docs / L"a.txt", small);
  write_bytes(docs / L"sub" / L"b.bin", large);

  const ExportManifest manifest = collect_export_manifest_v1(docs.wstring());
  expect(manifest.entries.size() == 5, "the tree must list five entries");
  expect(manifest.entries.front().relative_path == L"Docs" &&
             manifest.entries.front().kind == ExportEntryKind::directory,
         "the exported folder must come first");
  expect(manifest.data_bytes == small.size() + large.size(), "the data total must add up");

  const Bytes package = package_from(docs.wstring(), manifest);
  PackageSource source{package};
  ExportPackageReaderV1 reader = open_package(source);
  std::uint64_t last_done = 0;
  std::uint64_t last_total = 1;
  const ExportManifest verified = verify_export_package_v1(
      reader, {}, [&](const std::uint64_t done, const std::uint64_t total) {
        last_done = done;
        last_total = total;
      });
  expect(verified.entries == manifest.entries, "verification must return the manifest");
  expect(last_done == last_total && last_total == manifest.data_bytes, "progress must finish at the total");
  expect(source.cursor == package.size(), "verification must read the whole package");

  // The data region holds each file in manifest order.
  PackageSource again{package};
  ExportPackageReaderV1 content = open_package(again);
  const ExportManifest read = read_export_manifest_v1(content);
  Bytes expected;
  for (const auto& entry : read.entries) {
    if (entry.relative_path == L"Docs\\a.txt") expected.insert(expected.end(), small.begin(), small.end());
    if (entry.relative_path == L"Docs\\sub\\b.bin") expected.insert(expected.end(), large.begin(), large.end());
  }
  Bytes data(static_cast<std::size_t>(content.remaining()));
  content.read(data);
  expect(data == expected, "file contents must follow manifest order");
}

void test_single_file_and_changes_during_export() {
  const TemporaryDirectory temp;
  const auto path = temp.root() / L"note.txt";
  write_bytes(path, pattern(100));
  const ExportManifest manifest = collect_export_manifest_v1(path.wstring());
  expect(manifest.entries.size() == 1 && manifest.entries[0].kind == ExportEntryKind::file &&
             manifest.entries[0].size == 100,
         "a single file must be its own manifest");

  write_bytes(path, pattern(101));
  expect_throws<std::runtime_error>(
      [&] { static_cast<void>(package_from(path.wstring(), manifest)); },
      "a file that changed size must fail the export");

  expect_cancelled([&] { static_cast<void>(collect_export_manifest_v1(path.wstring(), [] { return true; })); },
                   "collection must honour cancellation");

  write_bytes(path, pattern(3 * kExportPackageV1ChunkSize));
  const ExportManifest large = collect_export_manifest_v1(path.wstring());
  int asked = 0;
  expect_cancelled([&] { static_cast<void>(package_from(path.wstring(), large, [&] { return ++asked > 2; })); },
                   "writing must honour cancellation");
}

void test_directory_swapped_for_a_junction_is_refused() {
  const TemporaryDirectory temp;
  const auto docs = temp.root() / L"Docs";
  const auto outside = temp.root() / L"outside";
  usb_vault_test::make_directory(docs / L"sub");
  usb_vault_test::make_directory(outside);
  write_bytes(docs / L"sub" / L"x.bin", pattern(3));
  write_bytes(outside / L"x.bin", pattern(3));
  const ExportManifest manifest = collect_export_manifest_v1(docs.wstring());
  std::filesystem::remove_all(docs / L"sub");
  if (!usb_vault_test::try_make_junction(docs / L"sub", outside)) {
    fail("could not build the junction fixture");
  }
  expect_throws<std::runtime_error>(
      [&] { static_cast<void>(package_from(docs.wstring(), manifest)); },
      "a directory swapped for a junction after the walk must be refused");
}

void test_links_are_refused() {
  const TemporaryDirectory temp;
  const auto docs = temp.root() / L"Docs";
  const auto outside = temp.root() / L"outside";
  usb_vault_test::make_directory(docs);
  usb_vault_test::make_directory(outside);
  write_bytes(outside / L"secret.txt", pattern(3));
  if (!usb_vault_test::try_make_junction(docs / L"link", outside)) {
    fail("could not build the junction fixture");
  }
  expect_throws<std::runtime_error>(
      [&] { static_cast<void>(collect_export_manifest_v1(docs.wstring())); },
      "a junction inside the source must be refused");
  expect_throws<std::runtime_error>(
      [&] { static_cast<void>(collect_export_manifest_v1((docs / L"link").wstring())); },
      "a junction as the source must be refused");
}

void test_verification_rejects_damage_and_honours_cancel() {
  const TemporaryDirectory temp;
  const auto path = temp.root() / L"big.bin";
  write_bytes(path, pattern(2 * kExportPackageV1ChunkSize));
  const ExportManifest manifest = collect_export_manifest_v1(path.wstring());
  const Bytes package = package_from(path.wstring(), manifest);

  Bytes damaged = package;
  damaged[damaged.size() - 100] ^= std::byte{4};
  PackageSource damaged_source{damaged};
  ExportPackageReaderV1 damaged_reader = open_package(damaged_source);
  expect_throws<ExportPackageRejected>(
      [&] { static_cast<void>(verify_export_package_v1(damaged_reader)); },
      "damage in the data region must fail verification");

  PackageSource cancelled_source{package};
  ExportPackageReaderV1 cancelled_reader = open_package(cancelled_source);
  int calls = 0;
  expect_cancelled([&] {
    static_cast<void>(verify_export_package_v1(cancelled_reader, [&] { return ++calls > 2; }));
  }, "verification must honour cancellation");
}

void test_entry_count_over_the_limit_is_rejected() {
  const ExportPackageKeyV1 key = ExportPackageKeyV1::derive(
      secure_text(kPassword), generate_export_package_v1_salt());
  Bytes stream(kExportPackageV1StreamHeaderSize);
  const std::string_view marker = "UVEXPKG1";
  std::copy_n(reinterpret_cast<const std::byte*>(marker.data()), marker.size(), stream.begin());
  stream[8] = std::byte{1};
  const std::uint64_t count = kExportManifestV1MaximumEntries + 1;
  for (std::size_t i = 0; i < 8; ++i) stream[32 + i] = static_cast<std::byte>((count >> (8 * i)) & 0xFF);
  Bytes package(key.salt().begin(), key.salt().end());
  Bytes sealed(stream.size() + kExportPackageV1TagSize);
  key.seal_chunk(0, true, stream, sealed);
  package.insert(package.end(), sealed.begin(), sealed.end());
  expect(package.size() == kExportPackageV1SaltSize + sealed.size(), "crafted layout");
  PackageSource source{package};
  ExportPackageReaderV1 reader = open_package(source);
  expect_throws<ExportPackageRejected>(
      [&] { static_cast<void>(read_export_manifest_v1(reader)); },
      "an entry count over the limit must be rejected before allocating");
}

}  // namespace

int main() {
  try {
    test_valid_manifests_pass_and_round_trip();
    test_invalid_manifests_are_rejected();
    test_malformed_manifest_bytes_are_rejected();
    test_directory_tree_exports_and_verifies();
    test_single_file_and_changes_during_export();
    test_links_are_refused();
    test_directory_swapped_for_a_junction_is_refused();
    test_verification_rejects_damage_and_honours_cancel();
    test_entry_count_over_the_limit_is_rejected();
  } catch (const std::exception& error) {
    fail(std::string("unexpected exception: ") + error.what());
  }
  std::cout << "export_manifest_v1_tests passed\n";
  return EXIT_SUCCESS;
}
