#include "usb_vault/backup/import_package_v1.h"

#include "usb_vault/backup/export_package_file.h"
#include "usb_vault/backup/export_package_v1.h"

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
}

}  // namespace

int main() {
  try {
    test_import_restores_the_tree_after_verifying();
    test_import_failures_leave_the_destination_as_it_was();
    test_a_single_file_package_restores_one_file();
  } catch (const std::exception& error) {
    fail(std::string("unexpected exception: ") + error.what());
  }
  std::cout << "import_package_v1_tests passed\n";
  return EXIT_SUCCESS;
}
