#include "usb_vault/backup/import_package_v1.h"

#include "usb_vault/backup/export_package_file.h"
#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/backup/transfer_package_v1.h"

#include "backup/transfer_package_v1_detail.h"

#include "volume_test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using usb_vault::backup::export_package_to_file_v1;
using usb_vault::backup::ExportPackageRejected;
using usb_vault::backup::ImportPackageV1;
using usb_vault::backup::kExportPackageV1ChunkSize;
using usb_vault::security::SecureBuffer;
using usb_vault_test::TemporaryDirectory;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const std::string& message) {
  if (!condition) fail(message);
}

template <typename Exception>
void expect_throws(const auto& action, const std::string& message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  } catch (const std::exception& error) {
    fail(message + " (threw " + error.what() + ")");
  }
  fail(message + " (did not throw)");
}

void expect_win32(const auto& action, const int code, const std::string& message) {
  try {
    action();
  } catch (const std::system_error& error) {
    expect(error.code().value() == code, message + " (code " + std::to_string(error.code().value()) + ")");
    return;
  } catch (const std::exception& error) {
    fail(message + " (threw " + error.what() + ")");
  }
  fail(message + " (did not throw)");
}

[[nodiscard]] SecureBuffer secure(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())), result.bytes().begin());
  return result;
}

[[nodiscard]] std::string pattern(const std::size_t size, const char seed) {
  std::string text(size, '\0');
  for (std::size_t i = 0; i < size; ++i) text[i] = static_cast<char>(seed + (i * 7) % 61);
  return text;
}

[[nodiscard]] std::size_t entries_in(const std::filesystem::path& directory) {
  return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(directory),
                                                std::filesystem::directory_iterator()));
}

// The destination's drive and its path below the root, as the session names
// a directory inside a mounted volume.
struct Destination final {
  wchar_t drive{};
  std::wstring relative;
};

[[nodiscard]] Destination destination_of(const std::filesystem::path& directory) {
  const std::wstring text = directory.wstring();
  return {text[0], text.substr(3)};
}

struct Fixture final {
  TemporaryDirectory temp;
  std::filesystem::path package = temp.root() / L"pkg";
  std::filesystem::path into = temp.root() / L"into";
  std::string small = pattern(9, 'a');
  std::string large = pattern(kExportPackageV1ChunkSize + 77, 'K');

  Fixture() {
    const auto docs = temp.root() / L"Docs";
    usb_vault_test::make_directory(docs / L"sub" / L"empty");
    usb_vault_test::write_text_file(docs / L"a.txt", small);
    usb_vault_test::write_text_file(docs / L"sub" / L"b.bin", large);
    std::filesystem::last_write_time(docs / L"a.txt",
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
    export_package_to_file_v1(docs.wstring(), package.wstring(), secure("import test password"));
    usb_vault_test::make_directory(into);
  }
};

void test_import_restores_the_tree_after_verifying() {
  Fixture fixture;
  ImportPackageV1 package(fixture.package.wstring());
  expect_throws<std::logic_error>(
      [&] { static_cast<void>(package.required_bytes()); },
      "the room needed must not be known before verification");
  const Destination destination = destination_of(fixture.into);
  expect_throws<std::logic_error>(
      [&] {
        static_cast<void>(package.write_into(secure("import test password"), destination.drive,
                                             destination.relative, {}, {}));
      },
      "writing must refuse to run before verification");

  unsigned int last_phase = 0;
  const auto& manifest = package.verify(secure("import test password"), {},
                                        [&](unsigned int phase, std::uint64_t, std::uint64_t) { last_phase = phase; });
  expect(last_phase == 1 && manifest.entries.size() == 5, "verification must report phase one");
  expect(entries_in(fixture.into) == 0, "verification must not write anything");
  expect(package.required_bytes() >= fixture.small.size() + fixture.large.size(),
         "the room needed must cover the data");

  // The package cannot change under an import that holds it.
  {
    std::ofstream writer(fixture.package, std::ios::binary | std::ios::app);
    expect(!writer.is_open(), "the package must be closed to writers during an import");
  }

  const std::wstring restored = package.write_into(
      secure("import test password"), destination.drive, destination.relative, {},
      [&](unsigned int phase, std::uint64_t, std::uint64_t) { last_phase = phase; });
  expect(restored == L"Docs" && last_phase == 2, "the restored name and phase two must be reported");
  expect(entries_in(fixture.into) == 1, "only the restored entry may remain");
  const auto docs = fixture.into / L"Docs";
  expect(usb_vault_test::read_text_file(docs / L"a.txt") == fixture.small &&
             usb_vault_test::read_text_file(docs / L"sub" / L"b.bin") == fixture.large &&
             std::filesystem::is_directory(docs / L"sub" / L"empty"),
         "the restored tree must match the exported one");
  const DWORD attributes = ::GetFileAttributesW(docs.wstring().c_str());
  expect((attributes & FILE_ATTRIBUTE_HIDDEN) == 0, "the restored entry must not stay hidden");
  expect(std::filesystem::last_write_time(docs / L"a.txt") <
             std::filesystem::file_time_type::clock::now() - std::chrono::hours(24),
         "the restored file must keep its modification time");
  package.set_restored_name(L"Renamed archive");
  expect(package.write_into(secure("import test password"), destination.drive,
                            destination.relative, {}, {}) == L"Renamed archive",
         "a restored folder did not use the override name");
  expect(package.verify(secure("import test password"), {}, {}).top_level_name() == L"Docs",
         "renaming must not mutate the authenticated manifest");
  expect(usb_vault_test::read_text_file(fixture.into / L"Renamed archive" / L"sub" / L"b.bin") == fixture.large,
         "renaming the top folder changed a child path or its contents");
}

void test_import_failures_leave_the_destination_as_it_was() {
  Fixture fixture;
  const Destination destination = destination_of(fixture.into);

  {
    ImportPackageV1 package(fixture.package.wstring());
    expect_throws<ExportPackageRejected>(
        [&] { static_cast<void>(package.verify(secure("import test passwore"), {}, {})); },
        "a wrong password must be rejected by verification");
  }

  usb_vault_test::make_directory(fixture.into / L"Docs");
  usb_vault_test::write_text_file(fixture.into / L"Docs" / L"mine.txt", "keep");
  {
    ImportPackageV1 package(fixture.package.wstring());
    static_cast<void>(package.verify(secure("import test password"), {}, {}));
    expect_win32([&] {
      static_cast<void>(package.write_into(secure("import test password"), destination.drive,
                                           destination.relative, {}, {}));
    }, ERROR_FILE_EXISTS, "an occupied name must be refused");
  }
  expect(entries_in(fixture.into) == 1 && entries_in(fixture.into / L"Docs") == 1 &&
             usb_vault_test::read_text_file(fixture.into / L"Docs" / L"mine.txt") == "keep",
         "an occupied name must be left untouched and not merged into");
  std::filesystem::remove_all(fixture.into / L"Docs");

  {
    ImportPackageV1 package(fixture.package.wstring());
    static_cast<void>(package.verify(secure("import test password"), {}, {}));
    int asked = 0;
    expect_win32([&] {
      static_cast<void>(package.write_into(secure("import test password"), destination.drive,
                                           destination.relative, [&] { return ++asked > 4; }, {}));
    }, ERROR_CANCELLED, "a cancelled import must report cancellation");
  }
  expect(entries_in(fixture.into) == 0, "a cancelled import must remove its staging entry");

  // A directory so deep that the restored paths cannot fit under it.
  auto deep = fixture.into;
  while (deep.wstring().size() < 240) deep /= L"dddddddddd";
  usb_vault_test::make_directory(deep);
  {
    ImportPackageV1 package(fixture.package.wstring());
    static_cast<void>(package.verify(secure("import test password"), {}, {}));
    const Destination deep_destination = destination_of(deep);
    expect_throws<std::invalid_argument>([&] {
      static_cast<void>(package.write_into(secure("import test password"), deep_destination.drive,
                                           deep_destination.relative, {}, {}));
    }, "paths too long for the destination must be refused before writing");
  }
  expect(entries_in(deep) == 0, "a refused import must create nothing");

  try {
    ImportPackageV1 directory(fixture.into.wstring());
    fail("a directory must not be accepted as a package");
  } catch (const std::system_error&) {
  } catch (const std::invalid_argument&) {
  }
}

void test_a_single_file_package_restores_one_file() {
  TemporaryDirectory temp;
  const auto source = temp.root() / L"note.txt";
  usb_vault_test::write_text_file(source, "only one");
  const auto package_path = temp.root() / L"note package";
  export_package_to_file_v1(source.wstring(), package_path.wstring(), secure("single file password"));
  const auto into = temp.root() / L"into";
  usb_vault_test::make_directory(into);
  ImportPackageV1 package(package_path.wstring());
  static_cast<void>(package.verify(secure("single file password"), {}, {}));
  expect(package.required_bytes(4096) >= 4096 && package.required_bytes(65536) >= 65536,
         "a single file must need at least one cluster");
  const Destination destination = destination_of(into);
  expect(package.write_into(secure("single file password"), destination.drive, destination.relative,
                            {}, {}) == L"note.txt",
         "the single file was not restored under its name");
  expect(entries_in(into) == 1 && usb_vault_test::read_text_file(into / L"note.txt") == "only one",
         "the single file was not restored");
  expect_throws<std::invalid_argument>([&] { package.set_restored_name(L"..\\escape"); },
                                      "the restored name must be a single component");
  package.set_restored_name(L"renamed.txt");
  expect(package.write_into(secure("single file password"), destination.drive, destination.relative,
                            {}, {}) == L"renamed.txt", "a file name override was ignored");
  expect(usb_vault_test::read_text_file(into / L"renamed.txt") == "only one", "a renamed file changed content");
  expect_win32([&] { static_cast<void>(package.write_into(secure("single file password"), destination.drive,
                              destination.relative, {}, {})); }, ERROR_FILE_EXISTS,
               "a renamed import must not overwrite another entry");
}

[[nodiscard]] usb_vault::contacts::ContactIdentity identity_of(const unsigned char fill) {
  SecureBuffer secret(32);
  for (std::size_t i = 0; i < secret.size(); ++i)
    secret.data()[i] = static_cast<std::byte>(fill + i);
  return usb_vault::contacts::ContactIdentity::derive(secret);
}

[[nodiscard]] std::vector<std::byte> file_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
  const auto bytes = std::as_bytes(std::span(raw));
  return {bytes.begin(), bytes.end()};
}

// A recipient holding the file key re-encrypts edited content under the same
// recipient block, keeping the original sender key and signature.
void write_forgery(const std::filesystem::path& genuine, const std::filesystem::path& forged,
                   const usb_vault::contacts::ContactIdentity& insider) {
  using namespace usb_vault::backup;
  const std::vector<std::byte> file = file_bytes(genuine);
  const std::span<const std::byte> prefix(file.data(), kTransferPackageV1PrefixSize);
  std::size_t at = 0;
  ExportPackageReaderV1 reader(file.size(), [&](std::span<std::byte> out) {
    std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(at), out.size(), out.begin());
    at += out.size();
  }, kTransferPackageV1PrefixSize, [&](std::span<const std::byte> block) {
    return detail::open_transfer_package_key(insider, block);
  });
  const ExportPackageV1Header header = reader.header();
  std::vector<std::byte> stream(static_cast<std::size_t>(reader.remaining()));
  reader.read(stream);
  stream[stream.size() - 64 - 3] ^= std::byte{1};  // The last data byte but two.
  std::ofstream out(forged, std::ios::binary);
  ExportPackageWriterV1 writer(detail::open_transfer_package_key(insider, prefix), header,
                               [&](std::span<const std::byte> bytes) {
                                 out.write(reinterpret_cast<const char*>(bytes.data()),
                                           static_cast<std::streamsize>(bytes.size()));
                               });
  writer.write(stream);
  writer.finish();
}

void test_transfer_packages_import_only_for_recipients() {
  using namespace usb_vault::backup;
  using usb_vault::contacts::ContactPublicKey;
  TemporaryDirectory temp;
  const auto docs = temp.root() / L"Docs";
  usb_vault_test::make_directory(docs / L"sub");
  const std::string small = pattern(9, 'a');
  const std::string large = pattern(kExportPackageV1ChunkSize + 77, 'K');
  usb_vault_test::write_text_file(docs / L"a.txt", small);
  usb_vault_test::write_text_file(docs / L"sub" / L"b.bin", large);
  const auto alice = identity_of(1);
  const auto bob = identity_of(2);
  const auto carol = identity_of(3);
  const auto dave = identity_of(4);
  const auto package_path = temp.root() / L"pkg";
  const std::vector<ExportSelectionSourceV1> sources{{docs.wstring(), L"Docs"}};
  const std::vector<ContactPublicKey> to{bob.public_key(), carol.public_key()};
  export_selection_to_file_v1(sources, package_path.wstring(), L"",
                              ExportRecipientsV1{.sender = alice, .recipients = to});
  const auto into = temp.root() / L"into";
  usb_vault_test::make_directory(into);
  const Destination destination = destination_of(into);

  {
    ImportPackageV1 package(package_path.wstring());
    expect(!package.verify_transfer(dave, {}, {}) && !package.verified_as_transfer(),
           "a transfer package verified for someone it is not addressed to");
    expect_throws<std::logic_error>([&] {
      static_cast<void>(package.write_into(dave, destination.drive, destination.relative, {}, {}));
    }, "an unverified transfer package was written");
    expect_throws<ExportPackageRejected>([&] {
      static_cast<void>(package.verify(secure("any password"), {}, {}));
    }, "a transfer package opened with a password");
  }

  ImportPackageV1 package(package_path.wstring());
  unsigned int phase = 0;
  expect(package.verify_transfer(bob, {}, [&](unsigned int p, auto, auto) { phase = p; }) &&
             phase == 1 && package.transfer_sender() == alice.public_key(),
         "a recipient could not verify the transfer package or see its sender");
  expect(entries_in(into) == 0, "verifying a transfer package wrote something");
  expect_throws<std::logic_error>([&] {
    static_cast<void>(package.write_into(secure("any password"), destination.drive,
                                         destination.relative, {}, {}));
  }, "a verified transfer package was written with a password");
  expect_throws<TransferPackageNotAddressed>([&] {
    static_cast<void>(package.write_into(dave, destination.drive, destination.relative, {}, {}));
  }, "a second pass opened for someone the package is not addressed to");
  expect(entries_in(into) == 0, "a refused second pass left something behind");
  expect(package.write_into(bob, destination.drive, destination.relative, {},
                            [&](unsigned int p, auto, auto) { phase = p; }) == L"Docs" &&
             phase == 2,
         "a verified transfer package was not restored");
  expect(usb_vault_test::read_text_file(into / L"Docs" / L"a.txt") == small &&
             usb_vault_test::read_text_file(into / L"Docs" / L"sub" / L"b.bin") == large,
         "a transfer import did not restore the exported tree");

  // A recipient's forgery authenticates but its signature does not.
  const auto forged_path = temp.root() / L"forged";
  write_forgery(package_path, forged_path, bob);
  ImportPackageV1 forged(forged_path.wstring());
  expect_throws<TransferPackageForged>([&] {
    static_cast<void>(forged.verify_transfer(carol, {}, {}));
  }, "a forged transfer package was accepted");
  expect(!forged.verified_as_transfer(), "a forged package stayed verified");
  expect(entries_in(into) == 1, "a forged package wrote something");

  // Damage: a recipient block that no slot opens is not addressed; a slot that
  // opens over damaged chunks is a damaged package.
  const auto damage = [&](const std::size_t at, const std::filesystem::path& to_path) {
    std::vector<std::byte> bytes = file_bytes(package_path);
    bytes[at] ^= std::byte{1};
    std::ofstream out(to_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  };
  const auto broken_block = temp.root() / L"broken-block";
  damage(5, broken_block);
  {
    ImportPackageV1 broken(broken_block.wstring());
    expect(!broken.verify_transfer(bob, {}, {}),
           "a package with a damaged one-time key was not 'not addressed'");
  }
  const auto broken_chunk = temp.root() / L"broken-chunk";
  damage(kTransferPackageV1PrefixSize + 100, broken_chunk);
  {
    ImportPackageV1 broken(broken_chunk.wstring());
    expect_throws<ExportPackageRejected>([&] {
      static_cast<void>(broken.verify_transfer(bob, {}, {}));
    }, "a damaged chunk behind an opening slot was not rejected");
  }

  // Mixed calls: a password verification clears the transfer state, and a
  // password-verified state is not a transfer one.
  ImportPackageV1 mixed(package_path.wstring());
  expect(mixed.verify_transfer(bob, {}, {}), "the mixed-order package did not verify");
  expect_throws<ExportPackageRejected>([&] {
    static_cast<void>(mixed.verify(secure("any password"), {}, {}));
  }, "a password opened the transfer package");
  expect(!mixed.verified_as_transfer(), "a failed password pass kept the transfer state");
  expect_throws<std::logic_error>([&] { static_cast<void>(mixed.transfer_sender()); },
                                  "the sender stayed readable after verification was reset");
  expect_throws<std::logic_error>([&] {
    static_cast<void>(mixed.write_into(bob, destination.drive, destination.relative, {}, {}));
  }, "a reset package was written as a transfer package");

  // A renamed transfer import restores under the new top-level name.
  expect(mixed.verify_transfer(bob, {}, {}), "the package did not verify again");
  mixed.set_restored_name(L"From Alice");
  expect(mixed.write_into(bob, destination.drive, destination.relative, {}, {}) == L"From Alice" &&
             usb_vault_test::read_text_file(into / L"From Alice" / L"a.txt") == small,
         "a renamed transfer import did not use the new name");
}

}  // namespace

int main() {
  try {
    test_import_restores_the_tree_after_verifying();
    test_import_failures_leave_the_destination_as_it_was();
    test_a_single_file_package_restores_one_file();
    test_transfer_packages_import_only_for_recipients();
  } catch (const std::exception& error) {
    fail(std::string("unexpected exception: ") + error.what());
  }
  std::cout << "import_package_v1_tests passed\n";
  return EXIT_SUCCESS;
}
