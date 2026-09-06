#include "usb_vault/application/container_index_store_v1.h"

#include "usb_vault/application/container_index_v1.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::container_index_v1_default_path;
using usb_vault::application::container_index_v1_mutable_slot;
using usb_vault::application::container_index_v1_slot;
using usb_vault::application::ensure_container_index_v1_file;
using usb_vault::application::kContainerIndexV1FileSize;
using usb_vault::application::kContainerIndexV1KeySize;
using usb_vault::application::kContainerIndexV1SlotCount;
using usb_vault::application::kContainerIndexV1SlotSize;
using usb_vault::application::read_container_index_v1_file;
using usb_vault::application::seal_container_index_entry_v1;
using usb_vault::application::try_open_container_index_entry_v1;
using usb_vault::application::write_container_index_v1_file;
using usb_vault::security::SecureBuffer;

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition) {
    fail(message);
  }
}

template <typename Callable>
void expect_throws(Callable &&callable, const char *const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::exception &) {
    return;
  }
  fail(message);
}

// A directory of its own per test, so nothing here can reach the index of the
// account running the tests.
class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1> temp_root{};
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(temp_root.size()),
                                        temp_root.data());
    if (length == 0 || length >= temp_root.size()) {
      fail("resolve the temporary directory");
    }

    std::array<wchar_t, MAX_PATH + 1> unique{};
    if (::GetTempFileNameW(temp_root.data(), L"uvi", 0, unique.data()) == 0) {
      fail("reserve a unique temporary path");
    }
    if (::DeleteFileW(unique.data()) == FALSE) {
      fail("release the reserved temporary path");
    }

    root_ = std::filesystem::path(unique.data()).concat(L".dir");
    std::filesystem::create_directories(root_);
  }

  ~TemporaryDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::wstring index_path() const {
    return (root_ / L"container_index.bin").wstring();
  }

private:
  std::filesystem::path root_;
};

using Key = std::array<std::byte, kContainerIndexV1KeySize>;

[[nodiscard]] Key test_key(const unsigned char seed) {
  Key key{};
  for (std::size_t index = 0; index < key.size(); ++index) {
    key[index] =
        static_cast<std::byte>(static_cast<unsigned char>(seed + (index * 5U)));
  }
  return key;
}

[[nodiscard]] std::size_t
count_files_in(const std::filesystem::path &directory) {
  std::size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file()) {
      ++count;
    }
  }
  return count;
}

void test_the_default_path_sits_under_local_app_data() {
  const std::wstring path = container_index_v1_default_path();
  expect(path.size() > 3 && path[1] == L':' && path[2] == L'\\',
         "the default index path is not absolute");
  expect(path.find(L"\\usb-vault\\") != std::wstring::npos,
         "the default index path is not under a product directory");
  expect(path.ends_with(L"container_index.bin"),
         "the default index path does not name the index file");
}

void test_first_run_creates_a_full_size_random_file() {
  const TemporaryDirectory directory;
  const std::wstring path = directory.index_path();

  expect(ensure_container_index_v1_file(path),
         "the first call did not report creating the file");
  expect(std::filesystem::file_size(path) == kContainerIndexV1FileSize,
         "the new file is not the full size");

  const std::vector<std::byte> contents = read_container_index_v1_file(path);
  // Every slot has to be filler drawn separately. A file of repeated bytes, or
  // one padded with zeroes, would announce that it holds nothing. Two slots
  // drawn independently do not agree on their first sixteen bytes, so a set of
  // those prefixes settles it without comparing every pair.
  std::set<std::array<std::byte, 16>> prefixes;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    std::array<std::byte, 16> prefix{};
    std::ranges::copy(container_index_v1_slot(contents, index).first(
                          prefix.size()),
                      prefix.begin());
    if (!prefixes.insert(prefix).second) {
      fail("two slots of a new file came out identical");
    }
  }

  expect(!ensure_container_index_v1_file(path),
         "a second call reported creating the file again");
  expect(std::ranges::equal(contents, read_container_index_v1_file(path)),
         "a second call rewrote a file that was already there");
}

void test_first_run_creates_the_directory_it_needs() {
  const TemporaryDirectory directory;
  const std::wstring path =
      (directory.root() / L"nested" / L"container_index.bin").wstring();

  expect(ensure_container_index_v1_file(path),
         "the file was not created inside a directory that did not exist yet");
  expect(std::filesystem::file_size(path) == kContainerIndexV1FileSize,
         "the file created in a new directory is not the full size");
}

void test_an_entry_survives_the_round_trip_through_the_file() {
  const TemporaryDirectory directory;
  const std::wstring path = directory.index_path();
  static_cast<void>(ensure_container_index_v1_file(path));

  const Key key = test_key(0x31);
  const std::wstring container = L"D:\\vaults\\stored.hc";
  const SecureBuffer sealed = seal_container_index_entry_v1(key, container);

  std::vector<std::byte> contents = read_container_index_v1_file(path);
  std::ranges::copy(sealed.bytes(),
                    container_index_v1_mutable_slot(contents, 42).begin());
  write_container_index_v1_file(path, contents);

  const std::vector<std::byte> reread = read_container_index_v1_file(path);
  const std::optional<std::wstring> opened = try_open_container_index_entry_v1(
      key, container_index_v1_slot(reread, 42));
  expect(opened.has_value() && *opened == container,
         "an entry did not survive a round trip through the file");
}

void test_a_rewrite_moves_only_the_slot_it_meant_to() {
  // Anyone able to compare two copies of the file over time sees exactly which
  // slots changed. If a rewrite refreshed the filler as well, the slots that
  // held still would be the occupied ones, and their count and positions are
  // the very thing the sealing is for.
  const TemporaryDirectory directory;
  const std::wstring path = directory.index_path();
  static_cast<void>(ensure_container_index_v1_file(path));

  const std::vector<std::byte> before = read_container_index_v1_file(path);
  const SecureBuffer sealed =
      seal_container_index_entry_v1(test_key(0x32), L"D:\\vaults\\one.hc");

  std::vector<std::byte> contents = before;
  std::ranges::copy(sealed.bytes(),
                    container_index_v1_mutable_slot(contents, 7).begin());
  write_container_index_v1_file(path, contents);

  const std::vector<std::byte> after = read_container_index_v1_file(path);
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const bool changed = !std::ranges::equal(
        container_index_v1_slot(before, index),
        container_index_v1_slot(after, index));
    if (index == 7) {
      expect(changed, "the slot that was written did not change");
    } else if (changed) {
      fail("a rewrite disturbed a slot it was not asked to touch");
    }
  }
}

void test_a_write_leaves_no_staging_file_behind() {
  const TemporaryDirectory directory;
  const std::wstring path = directory.index_path();
  static_cast<void>(ensure_container_index_v1_file(path));
  expect(count_files_in(directory.root()) == 1,
         "creating the file left something else in the directory");

  write_container_index_v1_file(path, read_container_index_v1_file(path));
  expect(count_files_in(directory.root()) == 1,
         "a write left a staging file beside the index");
}

void test_a_file_of_the_wrong_size_is_refused_and_kept() {
  // Starting over from a fresh random file would throw away records that the
  // password in hand cannot even see, so damage has to be reported rather than
  // paved over.
  const TemporaryDirectory directory;
  const std::wstring path = directory.index_path();
  const std::vector<std::byte> stub(100, std::byte{0x5A});
  {
    const std::filesystem::path stub_path(path);
    std::filesystem::create_directories(stub_path.parent_path());
    HANDLE handle =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(handle != INVALID_HANDLE_VALUE, "could not stage a damaged file");
    DWORD written = 0;
    static_cast<void>(::WriteFile(handle, stub.data(),
                                  static_cast<DWORD>(stub.size()), &written,
                                  nullptr));
    static_cast<void>(::CloseHandle(handle));
  }

  expect_throws([&] { static_cast<void>(ensure_container_index_v1_file(path)); },
                "a file of the wrong size was accepted");
  expect_throws([&] { static_cast<void>(read_container_index_v1_file(path)); },
                "a file of the wrong size was read");
  expect(std::filesystem::file_size(path) == stub.size(),
         "a file of the wrong size was replaced instead of refused");
}

void test_reading_a_file_that_is_not_there_reports_rather_than_invents() {
  const TemporaryDirectory directory;
  expect_throws(
      [&] {
        static_cast<void>(read_container_index_v1_file(directory.index_path()));
      },
      "reading a missing index file did not report");
}

void test_the_slot_views_tile_the_file_and_refuse_bad_input() {
  std::vector<std::byte> contents(kContainerIndexV1FileSize);
  const std::span<const std::byte> whole(contents);

  const auto first = container_index_v1_slot(whole, 0);
  const auto last =
      container_index_v1_slot(whole, kContainerIndexV1SlotCount - 1);
  expect(first.data() == whole.data(), "the first slot does not start the file");
  expect(first.size() == kContainerIndexV1SlotSize,
         "a slot view is not one slot long");
  expect(last.data() + last.size() == whole.data() + whole.size(),
         "the last slot does not end the file");

  expect_throws(
      [&] {
        static_cast<void>(
            container_index_v1_slot(whole, kContainerIndexV1SlotCount));
      },
      "a slot index past the end was accepted");
  expect_throws(
      [&] {
        static_cast<void>(container_index_v1_mutable_slot(
            std::span(contents), kContainerIndexV1SlotCount));
      },
      "a mutable slot index past the end was accepted");

  std::vector<std::byte> partial(kContainerIndexV1FileSize - 1);
  expect_throws(
      [&] {
        static_cast<void>(
            container_index_v1_slot(std::span<const std::byte>(partial), 0));
      },
      "a buffer shorter than the file was accepted");
  expect_throws(
      [&] {
        write_container_index_v1_file(L"unused", std::span(partial));
      },
      "a write of something other than a whole file was accepted");
}

} // namespace

int main() {
  test_the_default_path_sits_under_local_app_data();
  test_first_run_creates_a_full_size_random_file();
  test_first_run_creates_the_directory_it_needs();
  test_an_entry_survives_the_round_trip_through_the_file();
  test_a_rewrite_moves_only_the_slot_it_meant_to();
  test_a_write_leaves_no_staging_file_behind();
  test_a_file_of_the_wrong_size_is_refused_and_kept();
  test_reading_a_file_that_is_not_there_reports_rather_than_invents();
  test_the_slot_views_tile_the_file_and_refuse_bad_input();
  std::cout << "container_index_store_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
