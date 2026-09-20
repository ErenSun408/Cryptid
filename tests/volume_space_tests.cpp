#include "usb_vault/volume/volume_space.h"

#include <Windows.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Exception, typename Action>
void require_throws(Action&& action, const char* const message) {
  try {
    action();
  } catch (const Exception&) {
    return;
  }
  throw std::runtime_error(message);
}

// The drive this test is running from, so the fixture is a volume that exists
// on whichever machine runs it.
[[nodiscard]] wchar_t local_drive_letter() {
  const std::wstring root = std::filesystem::current_path().root_name().native();
  require(root.size() == 2 && root[1] == L':',
          "the working directory is not on a lettered drive");
  return root[0];
}

void test_space_answers_for_a_real_volume() {
  const usb_vault::volume::VolumeSpace space =
      usb_vault::volume::volume_space(local_drive_letter());
  require(space.total_bytes > 0, "a mounted volume reported no capacity");
  require(space.free_bytes <= space.total_bytes,
          "free space exceeded the volume");
}

void test_a_letter_is_a_letter_whatever_its_case() {
  const wchar_t letter = local_drive_letter();
  const wchar_t lower = static_cast<wchar_t>(letter - L'A' + L'a');
  require(usb_vault::volume::volume_space(lower).total_bytes ==
              usb_vault::volume::volume_space(letter).total_bytes,
          "the same volume answered differently by case");
}

void test_what_is_not_a_drive_letter_is_refused() {
  for (const wchar_t candidate : {L'1', L'\\', L':', wchar_t{}}) {
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(usb_vault::volume::volume_space(candidate));
        },
        "a volume was queried by something that is not a drive letter");
    require_throws<std::invalid_argument>(
        [&] {
          static_cast<void>(
              usb_vault::volume::scan_volume_tail_space(candidate));
        },
        "a volume was scanned by something that is not a drive letter");
  }
}

void test_a_volume_that_is_not_there_is_a_system_error() {
  const DWORD mounted = ::GetLogicalDrives();
  for (wchar_t letter = L'Z'; letter >= L'A'; --letter) {
    if ((mounted & (1UL << (letter - L'A'))) != 0) continue;
    // Refused rather than answered with zero: nothing was measured, and a
    // zero here would read as a volume with no room left.
    require_throws<std::system_error>(
        [letter] {
          static_cast<void>(usb_vault::volume::volume_space(letter));
        },
        "a drive letter nothing answers to reported space");
    return;
  }
}

}  // namespace

int main() {
  try {
    test_space_answers_for_a_real_volume();
    test_a_letter_is_a_letter_whatever_its_case();
    test_what_is_not_a_drive_letter_is_refused();
    test_a_volume_that_is_not_there_is_a_system_error();
    std::cout << "volume_space_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "volume_space_tests failed: " << error.what() << '\n';
    return 1;
  }
}
