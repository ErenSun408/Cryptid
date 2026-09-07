#include "usb_vault/application/product_settings_v1.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using usb_vault::application::product_settings_v1_default_path;
using usb_vault::application::remember_container_paths_v1;
using usb_vault::application::set_remember_container_paths_v1;

[[noreturn]] void fail(const char *const message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void expect(const bool condition, const char *const message) {
  if (!condition) {
    fail(message);
  }
}

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
    if (::GetTempFileNameW(temp_root.data(), L"uvs", 0, unique.data()) == 0) {
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
  [[nodiscard]] std::wstring settings_path() const {
    return (root_ / L"settings.txt").wstring();
  }

private:
  std::filesystem::path root_;
};

void write_raw(const std::wstring &path, const std::string_view content) {
  const HANDLE handle =
      ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  expect(handle != INVALID_HANDLE_VALUE, "could not stage a settings file");
  DWORD written = 0;
  if (!content.empty()) {
    static_cast<void>(::WriteFile(handle, content.data(),
                                  static_cast<DWORD>(content.size()), &written,
                                  nullptr));
  }
  static_cast<void>(::CloseHandle(handle));
}

[[nodiscard]] std::size_t
count_files_in(const std::filesystem::path &directory) {
  std::size_t count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file()) ++count;
  }
  return count;
}

void test_the_default_path_sits_under_local_app_data() {
  const std::wstring path = product_settings_v1_default_path();
  expect(path.size() > 3 && path[1] == L':' && path[2] == L'\\',
         "the default settings path is not absolute");
  expect(path.find(L"\\usb-vault\\") != std::wstring::npos,
         "the default settings path is not under a product directory");
  expect(path.ends_with(L"settings.txt"),
         "the default settings path does not name the settings file");
}

void test_a_first_run_remembers_by_default() {
  const TemporaryDirectory directory;
  expect(remember_container_paths_v1(directory.settings_path()),
         "a machine that has kept nothing did not take the product default");
}

void test_a_choice_survives_and_can_be_taken_back() {
  const TemporaryDirectory directory;
  const std::wstring path = directory.settings_path();

  set_remember_container_paths_v1(path, false);
  expect(!remember_container_paths_v1(path),
         "turning remembering off did not stick");
  set_remember_container_paths_v1(path, true);
  expect(remember_container_paths_v1(path),
         "turning remembering back on did not stick");

  expect(count_files_in(directory.root()) == 1,
         "writing the choice left a staging file behind");
}

void test_writing_makes_the_directory_it_needs() {
  const TemporaryDirectory directory;
  const std::wstring path =
      (directory.root() / L"nested" / L"settings.txt").wstring();
  set_remember_container_paths_v1(path, false);
  expect(!remember_container_paths_v1(path),
         "a choice written into a new directory did not come back");
}

void test_a_choice_that_cannot_be_read_answers_no() {
  // Absence and damage are deliberately different. Nobody has chosen on a
  // first run, so the default stands; damage means a choice was made and is
  // now unreadable, and the only safe reading of that is the one that writes
  // nothing down.
  const TemporaryDirectory directory;
  const std::wstring path = directory.settings_path();

  write_raw(path, "");
  expect(!remember_container_paths_v1(path),
         "an empty settings file was taken for a first run");

  write_raw(path, "# usb-vault settings\r\nsomething-else=1\r\n");
  expect(!remember_container_paths_v1(path),
         "a settings file without the key was taken for a first run");

  write_raw(path, std::string(8192, 'x'));
  expect(!remember_container_paths_v1(path),
         "a file far too large to be settings was parsed anyway");

  // And a readable one still answers what it says.
  set_remember_container_paths_v1(path, true);
  expect(remember_container_paths_v1(path),
         "a rewritten choice did not replace the damaged file");
}

} // namespace

int main() {
  test_the_default_path_sits_under_local_app_data();
  test_a_first_run_remembers_by_default();
  test_a_choice_survives_and_can_be_taken_back();
  test_writing_makes_the_directory_it_needs();
  test_a_choice_that_cannot_be_read_answers_no();
  std::cout << "product_settings_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
