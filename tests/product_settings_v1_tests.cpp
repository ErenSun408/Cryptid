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

using usb_vault::application::mount_imported_containers_v1;
using usb_vault::application::product_settings_v1_default_path;
using usb_vault::application::remember_container_paths_v1;
using usb_vault::application::set_mount_imported_containers_v1;
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

void test_mounting_imported_containers_is_off_until_it_is_asked_for() {
  const TemporaryDirectory directory;
  const std::wstring path = directory.settings_path();

  // Unlike remembering paths, this one has no gap between absence and damage:
  // the product default is already the answer that opens nothing.
  expect(!mount_imported_containers_v1(path),
         "a first run offered to mount imported containers");
  write_raw(path, "# usb-vault settings\r\nremember-container-paths=1\r\n");
  expect(!mount_imported_containers_v1(path),
         "a settings file from before this setting answered yes");

  set_mount_imported_containers_v1(path, true);
  expect(mount_imported_containers_v1(path),
         "the choice to mount imported containers was not kept");
  set_mount_imported_containers_v1(path, false);
  expect(!mount_imported_containers_v1(path),
         "the choice could not be taken back");
}

void test_writing_one_setting_carries_the_other_across() {
  // Both settings share one file, so a write has to be a read-modify-write.
  // Losing the neighbour would silently undo a choice the user made.
  const TemporaryDirectory directory;
  const std::wstring path = directory.settings_path();

  set_remember_container_paths_v1(path, false);
  set_mount_imported_containers_v1(path, true);
  expect(!remember_container_paths_v1(path),
         "writing one setting reset the other to its default");
  expect(mount_imported_containers_v1(path),
         "the setting just written did not survive");

  set_remember_container_paths_v1(path, true);
  expect(mount_imported_containers_v1(path),
         "writing the neighbour back dropped this setting");
  expect(remember_container_paths_v1(path), "the neighbour did not change");
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

void test_neutral_preferences_round_trip_and_reject_malformed_force() {
  using namespace usb_vault::application;
  const TemporaryDirectory directory;
  const auto path = directory.settings_path();
  expect(product_preference_v1(path, ProductPreference::force_unmount) == 0 &&
         product_preference_v1(path, ProductPreference::auto_mount) == 0 &&
         product_preference_v1(path, ProductPreference::window_size) == 1 &&
         product_preference_v1(path, ProductPreference::operation_logging) == 1, "new defaults changed");
  set_product_preference_v1(path, ProductPreference::force_unmount, 1);
  set_product_preference_v1(path, ProductPreference::auto_mount, 1);
  set_product_preference_v1(path, ProductPreference::operation_logging, 0);
  set_product_preference_v1(path, ProductPreference::window_size, 3);
  set_remember_container_paths_v1(path, false);
  set_mount_imported_containers_v1(path, true);
  expect(product_preference_v1(path, ProductPreference::force_unmount) == 1 &&
         product_preference_v1(path, ProductPreference::auto_mount) == 1 &&
         product_preference_v1(path, ProductPreference::operation_logging) == 0 &&
         product_preference_v1(path, ProductPreference::window_size) == 3 &&
         !remember_container_paths_v1(path) && mount_imported_containers_v1(path), "updating neighbours lost preferences");
  for (const auto* text : {"force-unmount=10\n", "xforce-unmount=1\n", "force-unmount=1\nforce-unmount=0\n", "force-unmount=yes\n"}) {
    write_raw(path, text);
    expect(product_preference_v1(path, ProductPreference::force_unmount) == 0, "malformed force preference enabled force");
  }
  write_raw(path, "remember-container-paths=1\nmount-imported-containers=0\n");
  expect(product_preference_v1(path, ProductPreference::auto_mount) == 0, "old settings enabled automatic mounting");
  for (const auto* text : {"auto-mount=10\n", "xauto-mount=1\n", "auto-mount=1\nauto-mount=0\n", "auto-mount=yes\n"}) {
    write_raw(path, text);
    expect(product_preference_v1(path, ProductPreference::auto_mount) == 0, "malformed preference enabled automatic mounting");
  }
  expect(product_preference_v1(path, ProductPreference::window_size) == 1, "old file migration lost default");
  bool rejected = false;
  try { set_product_preference_v1(path, ProductPreference::window_size, 4); } catch (...) { rejected = true; }
  expect(rejected, "invalid preset admitted");
  const HANDLE locked = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  expect(locked != INVALID_HANDLE_VALUE, "could not lock settings fixture");
  rejected = false;
  try { set_product_preference_v1(path, ProductPreference::force_unmount, 1); } catch (...) { rejected = true; }
  ::CloseHandle(locked);
  expect(rejected && product_preference_v1(path, ProductPreference::force_unmount) == 0, "failed write changed stored force");
  set_product_preference_v1(path, ProductPreference::auto_mount, 1);
  set_product_preference_v1(path, ProductPreference::auto_mount, 0);
  expect(product_preference_v1(path, ProductPreference::auto_mount) == 0, "automatic mounting could not be disabled");
}

void test_exit_hotkey_settings() {
  using namespace usb_vault::application;
  const TemporaryDirectory directory;
  const auto path = directory.settings_path();
  expect(exit_hotkey_settings_v1(path) == ExitHotkeySettings{}, "missing settings enabled hotkey");
  const ExitHotkeySettings configured{true, 65, 5};
  set_exit_hotkey_settings_v1(path, configured);
  set_product_preference_v1(path, ProductPreference::window_size, 2);
  set_remember_container_paths_v1(path, false);
  set_mount_imported_containers_v1(path, true);
  expect(exit_hotkey_settings_v1(path) == configured, "other settings lost hotkey");
  set_exit_hotkey_settings_v1(path, {false, 65, 5});
  expect(exit_hotkey_settings_v1(path) == ExitHotkeySettings{false, 65, 5} &&
         product_preference_v1(path, ProductPreference::window_size) == 2 &&
         !remember_container_paths_v1(path) && mount_imported_containers_v1(path), "hotkey lost neighbours");
  for (auto invalid : {"exit-hotkey=1,123,0\n", "exit-hotkey=1,123,11\n", "exit-hotkey=1,17,3\n",
                       "exit-hotkey=1,123,03\n", "exit-hotkey=1,123,3,x\n", "exit-hotkey=2,123,3\n",
                       "exit-hotkey=1,123,3\nexit-hotkey=0,123,3\n", "exit-hotkey=1,123\n",
                       "exit-hotkey=1,123,42949672960\n", "exit-hotkey=1,123, 3\n"}) {
    write_raw(path, invalid);
    expect(exit_hotkey_settings_v1(path) == ExitHotkeySettings{}, "malformed config activated hotkey");
  }
  for (unsigned int key : {8U,9U,13U,37U,46U,103U,135U,144U,186U,226U}) {
    set_exit_hotkey_settings_v1(path, {true,key,2});
    expect(exit_hotkey_settings_v1(path) == ExitHotkeySettings{true,key,2}, "extended key lost on reload");
  }
  bool rejected = false;
  try { set_exit_hotkey_settings_v1(path, {true, 17, 2}); } catch (...) { rejected = true; }
  expect(rejected, "modifier admitted as single key");
  write_raw(path, "exit-hotkey=1,123,3\r\n");
  expect(exit_hotkey_settings_v1(path).enabled, "valid CRLF failed");
}

int main() {
  {
    using namespace usb_vault::application;
    const TemporaryDirectory directory;
    const auto path = directory.settings_path();
    expect(product_preference_v1(path, ProductPreference::container_extension) == 0, "extension default is not sys");
    for (unsigned int value = 0; value < 4; ++value) {
      set_product_preference_v1(path, ProductPreference::container_extension, value);
      set_exit_hotkey_settings_v1(path, {true,123,3});
      set_remember_container_paths_v1(path, false);
      set_product_preference_v1(path, ProductPreference::close_to_tray, 1);
      expect(product_preference_v1(path, ProductPreference::container_extension) == value &&
             exit_hotkey_settings_v1(path).enabled && !remember_container_paths_v1(path), "extension or neighbors lost");
    }
    for (const auto text : {"container-extension=4\n", "container-extension=03\n", "xcontainer-extension=0\n",
                           "container-extension=0\ncontainer-extension=2\n", "container-extension=yes\n"}) {
      write_raw(path, text);
      expect(product_preference_v1(path, ProductPreference::container_extension) == 0, "damaged extension did not fall back to sys");
    }
    bool rejected = false;
    try { set_product_preference_v1(path, ProductPreference::container_extension, 4); } catch (...) { rejected = true; }
    expect(rejected, "unsupported extension preference admitted");
  }
  {
    using namespace usb_vault::application;
    const TemporaryDirectory directory;
    const auto path = directory.settings_path();
    expect(product_preference_v1(path, ProductPreference::close_to_tray) == 0, "tray default not off");
    set_product_preference_v1(path, ProductPreference::close_to_tray, 1);
    set_exit_hotkey_settings_v1(path, {true, 123, 3});
    set_remember_container_paths_v1(path, false);
    expect(product_preference_v1(path, ProductPreference::close_to_tray) == 1, "neighbour lost tray choice");
    set_product_preference_v1(path, ProductPreference::close_to_tray, 0);
    expect(exit_hotkey_settings_v1(path).enabled && !remember_container_paths_v1(path), "tray update lost neighbours");
    for (const auto text : {"close-to-tray=1\nclose-to-tray=0\n", "close-to-tray=10\n", "xclose-to-tray=1\n", "close-to-tray=yes\n"}) {
      write_raw(path, text);
      expect(product_preference_v1(path, ProductPreference::close_to_tray) == 0, "damaged settings retained mounted session");
    }
  }
  test_exit_hotkey_settings();
  test_neutral_preferences_round_trip_and_reject_malformed_force();
  test_the_default_path_sits_under_local_app_data();
  test_a_first_run_remembers_by_default();
  test_a_choice_survives_and_can_be_taken_back();
  test_writing_makes_the_directory_it_needs();
  test_mounting_imported_containers_is_off_until_it_is_asked_for();
  test_writing_one_setting_carries_the_other_across();
  test_a_choice_that_cannot_be_read_answers_no();
  std::cout << "product_settings_v1_tests: PASS\n";
  return EXIT_SUCCESS;
}
