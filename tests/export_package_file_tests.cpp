#include "usb_vault/backup/export_package_file.h"

#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/backup/import_package_v1.h"

#include "volume_test_support.h"

#include <algorithm>
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
using usb_vault::backup::ExportPackageKeyV1;
using usb_vault::backup::ExportPackageReaderV1;
using usb_vault::backup::ExportPackageV1Salt;
using usb_vault::backup::kExportPackageV1ChunkSize;
using usb_vault::backup::verify_export_package_v1;
using usb_vault::security::SecureBuffer;
using usb_vault_test::TemporaryDirectory;

constexpr std::string_view kPassword = "file export password";

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const std::string& message) {
  if (!condition) fail(message);
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

[[nodiscard]] SecureBuffer password() {
  SecureBuffer result(kPassword.size());
  std::ranges::copy(std::as_bytes(std::span(kPassword.data(), kPassword.size())),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] std::size_t entries_in(const std::filesystem::path& directory) {
  return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(directory),
                                                std::filesystem::directory_iterator()));
}

void make_source(const std::filesystem::path& root) {
  usb_vault_test::make_directory(root / L"sub");
  usb_vault_test::make_file(root / L"a.txt", 10);
  usb_vault_test::make_file(root / L"sub" / L"b.bin", 2 * kExportPackageV1ChunkSize + 3);
}

void test_export_publishes_one_complete_package() {
  const TemporaryDirectory temp;
  make_source(temp.root() / L"Docs");
  usb_vault_test::make_directory(temp.root() / L"out");
  const auto destination = temp.root() / L"out" / L"Docs backup";
  std::uint64_t last_done = 0;
  std::uint64_t last_total = 1;
  export_package_to_file_v1((temp.root() / L"Docs").wstring(), destination.wstring(), password(), {},
                            [&](const std::uint64_t done, const std::uint64_t total) {
                              last_done = done;
                              last_total = total;
                            });
  expect(entries_in(temp.root() / L"out") == 1, "only the package must remain in the destination");
  expect(last_done == last_total && last_total == 2 * kExportPackageV1ChunkSize + 13,
         "progress must reach the data total");
  const DWORD attributes = ::GetFileAttributesW(destination.wstring().c_str());
  expect(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) == 0,
         "the published package must not stay hidden");

  std::ifstream input(destination, std::ios::binary);
  const std::uint64_t size = std::filesystem::file_size(destination);
  ExportPackageReaderV1 reader(
      size,
      [&](const std::span<std::byte> out) {
        input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (input.gcount() != static_cast<std::streamsize>(out.size())) throw std::runtime_error("short");
      },
      [](const ExportPackageV1Salt& salt) { return ExportPackageKeyV1::derive(password(), salt); });
  const auto manifest = verify_export_package_v1(reader);
  expect(manifest.entries.size() == 4 && manifest.top_level_name() == L"Docs",
         "the published package must verify");
}

void test_failures_leave_nothing_behind() {
  const TemporaryDirectory temp;
  make_source(temp.root() / L"Docs");
  const auto out = temp.root() / L"out";
  usb_vault_test::make_directory(out);
  const std::wstring source = (temp.root() / L"Docs").wstring();

  usb_vault_test::make_file(out / L"taken", 1);
  expect_win32([&] { export_package_to_file_v1(source, (out / L"taken").wstring(), password()); },
               ERROR_FILE_EXISTS, "an occupied destination must be refused");
  expect(std::filesystem::file_size(out / L"taken") == 1 && entries_in(out) == 1,
         "an occupied destination must be left untouched");
  std::filesystem::remove(out / L"taken");

  int asked = 0;
  expect_win32([&] {
    export_package_to_file_v1(source, (out / L"pkg").wstring(), password(), [&] { return ++asked > 4; });
  }, ERROR_CANCELLED, "a cancelled export must report cancellation");
  expect(entries_in(out) == 0, "a cancelled export must leave no file");

  if (!usb_vault_test::try_make_junction(temp.root() / L"Docs" / L"link", out)) {
    fail("could not build the junction fixture");
  }
  try {
    export_package_to_file_v1(source, (out / L"pkg").wstring(), password());
    fail("a source holding a link must be refused");
  } catch (const std::runtime_error&) {
  }
  expect(entries_in(out) == 0, "a refused export must leave no file");

  try {
    export_package_to_file_v1(source, (out.wstring() + L"\\"), password());
    fail("a destination without a name must be refused");
  } catch (const std::invalid_argument&) {
  }
}

void test_grouped_exports_preserve_names_and_cleanup() {
  using namespace usb_vault::backup;
  const TemporaryDirectory temp;
  const auto out = temp.root() / L"out";
  usb_vault_test::make_directory(out);
  usb_vault_test::make_directory(temp.root() / L"A");
  usb_vault_test::make_directory(temp.root() / L"B");
  usb_vault_test::make_file(temp.root() / L"A" / L"same.txt", 13);
  usb_vault_test::make_file(temp.root() / L"B" / L"same.txt", 27);
  const std::vector<ExportSelectionSourceV1> sources{
      {(temp.root() / L"A" / L"same.txt").wstring(), L"Vault A\\same.txt"},
      {(temp.root() / L"B" / L"same.txt").wstring(), L"Vault B\\same.txt"}};
  const auto plan = collect_export_selection_v1(sources, L"Combined");
  expect(plan.manifest.entries.size() == 5 && plan.manifest.data_bytes == 40,
      "a virtual folder must contain both vault folders and duplicate-named projects");
  bool staging_pinned = false;
  export_selection_to_file_v1(sources, (out / L"Plain").wstring(), L"Combined", false, SecureBuffer{}, {},
      [&](auto done, auto) {
        if (done != 0) return;
        for (const auto& entry : std::filesystem::directory_iterator(out)) {
          expect(!usb_vault_test::try_make_junction(entry.path(), temp.root() / L"B"),
              "a pinned export root must deny reparse-point mutation");
          staging_pinned = true;
        }
      });
  expect(staging_pinned, "the plaintext writer must report progress with its root pinned");
  expect(std::filesystem::file_size(out / L"Plain" / L"Vault A" / L"same.txt") == 13 &&
      std::filesystem::file_size(out / L"Plain" / L"Vault B" / L"same.txt") == 27,
      "plaintext groups must preserve same-named projects from both vaults");
  expect(entries_in(out) == 1, "plaintext staging must disappear after publication");
  const auto package = out / L"Cipher";
  export_selection_to_file_v1(sources, package.wstring(), L"Combined", true, password());
  ImportPackageV1 imported(package.wstring());
  const auto& restored_manifest = imported.verify(password(), {}, {});
  expect(restored_manifest.entries.size() == 5 && restored_manifest.data_bytes == 40 &&
      restored_manifest.top_level_name() == L"Combined", "a grouped encrypted package must verify");
  usb_vault_test::make_directory(temp.root() / L"restored");
  static_cast<void>(imported.write_into(password(), temp.root().wstring()[0],
      (temp.root() / L"restored").wstring().substr(3), {}, {}));
  expect(std::filesystem::file_size(temp.root() / L"restored" / L"Combined" / L"Vault B" / L"same.txt") == 27,
      "a grouped package must restore its virtual directory tree");
  expect_win32([&] { export_selection_to_file_v1(sources, (out / L"Plain").wstring(),
      L"Combined", false, SecureBuffer{}); }, ERROR_FILE_EXISTS, "grouped export must never replace a destination");
  const auto before = entries_in(out);
  bool copying = false;
  expect_win32([&] { export_selection_to_file_v1(sources, (out / L"Cancelled").wstring(),
      L"Combined", false, SecureBuffer{}, [&] { return copying; },
      [&](auto done, auto) { if (done != 0) copying = true; }); }, ERROR_CANCELLED,
      "cancelled plaintext groups must stop");
  expect(entries_in(out) == before, "cancelled plaintext groups must remove only their staging directory");
  bool changed = false;
  try {
    export_selection_to_file_v1(sources, (out / L"Changed").wstring(), L"Combined", false, SecureBuffer{}, {},
        [&](auto, auto) {
          if (!changed) { changed = true; usb_vault_test::make_file(temp.root() / L"A" / L"same.txt", 14); }
        });
    fail("changed plaintext sources must be rejected");
  } catch (const std::runtime_error&) {}
  expect(entries_in(out) == before, "source changes must remove the incomplete directory");
  usb_vault_test::make_directory(temp.root() / L"Tree" / L"empty");
  usb_vault_test::make_file(temp.root() / L"Tree" / L"inside", 0);
  const std::vector<ExportSelectionSourceV1> tree{{(temp.root() / L"Tree").wstring(), L"Vault\\Tree"}};
  export_selection_to_file_v1(tree, (out / L"Tree").wstring(), L"Combined", false, SecureBuffer{});
  expect(std::filesystem::is_directory(out / L"Tree" / L"Vault" / L"Tree" / L"empty") &&
      std::filesystem::file_size(out / L"Tree" / L"Vault" / L"Tree" / L"inside") == 0,
      "grouped exports must retain empty directories and zero-byte files");
  if (!usb_vault_test::try_make_junction(temp.root() / L"Tree" / L"link", temp.root() / L"B"))
    fail("could not build the grouped export junction fixture");
  const auto before_link = entries_in(out);
  try {
    export_selection_to_file_v1(tree, (out / L"Linked").wstring(), L"Combined", false, SecureBuffer{});
    fail("grouped exports must refuse source junctions");
  } catch (const std::runtime_error&) {}
  expect(entries_in(out) == before_link, "refused source junctions must leave no export");
  const std::vector<ExportSelectionSourceV1> through_link{
      {(temp.root() / L"Tree" / L"link" / L"same.txt").wstring(), L"Vault\\same.txt"}};
  try {
    export_selection_to_file_v1(through_link, (out / L"Ancestor").wstring(), L"Combined", false, SecureBuffer{});
    fail("grouped exports must refuse junctions above a selected source");
  } catch (const std::runtime_error&) {}
  expect(entries_in(out) == before_link, "source ancestor links must leave no export");
  auto invalid = sources; invalid[1].output_name = L"Vault A\\same.txt";
  try { static_cast<void>(collect_export_selection_v1(invalid, L"Combined"));
    fail("case-insensitive duplicate virtual paths must be refused");
  } catch (const std::invalid_argument&) {}
  invalid[1].output_name = L"..\\escape";
  try { static_cast<void>(collect_export_selection_v1(invalid, L"Combined"));
    fail("virtual parent traversal must be refused");
  } catch (const std::invalid_argument&) {}
}

}  // namespace

int main() {
  try {
    test_export_publishes_one_complete_package();
    test_failures_leave_nothing_behind();
    test_grouped_exports_preserve_names_and_cleanup();
  } catch (const std::exception& error) {
    fail(std::string("unexpected exception: ") + error.what());
  }
  std::cout << "export_package_file_tests passed\n";
  return EXIT_SUCCESS;
}
