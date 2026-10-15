#include "usb_vault/backup/export_package_file.h"

#include "usb_vault/backup/export_package_v1.h"

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

}  // namespace

int main() {
  try {
    test_export_publishes_one_complete_package();
    test_failures_leave_nothing_behind();
  } catch (const std::exception& error) {
    fail(std::string("unexpected exception: ") + error.what());
  }
  std::cout << "export_package_file_tests passed\n";
  return EXIT_SUCCESS;
}
