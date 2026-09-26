// A mount takes a random free drive letter rather than always the same one.
// These assertions hold the two properties that matter: it is never a letter
// something else is on, and over many draws it is not stuck on a few.
#include "volume/drive_letter_pool.h"
#include "usb_vault/volume/volume_provider.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::volume::choose_free_drive_letter;
using usb_vault::volume::kMountDriveCount;
using usb_vault::volume::uniform_random_below;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::uint32_t bit(const wchar_t letter) {
  return std::uint32_t{1} << static_cast<std::uint32_t>(letter - L'A');
}

// Every letter D..Z busy except the ones named.
[[nodiscard]] std::uint32_t all_busy_but(const std::initializer_list<wchar_t> free) {
  std::uint32_t used = 0;
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) used |= bit(letter);
  for (const wchar_t letter : free) used &= ~bit(letter);
  return used;
}

void test_driver_only_mounts_are_excluded_from_the_free_set() {
  using usb_vault::volume::MountedVolume;
  using usb_vault::volume::occupied_drive_letters;
  // DOS offers Q and Z, but Q is already mounted in the driver.
  const auto logical = all_busy_but({L'Q', L'Z'});
  const std::array mounts{MountedVolume{.drive_letter = L'Q'},
                          MountedVolume{.drive_letter = L'Q'},
                          MountedVolume{.drive_letter = L'D'},
                          MountedVolume{.drive_letter = wchar_t{}},
                          MountedVolume{.drive_letter = L'@'},
                          MountedVolume{.drive_letter = L'['},
                          MountedVolume{.drive_letter = L'z'}};
  const auto used = occupied_drive_letters(logical, mounts);
  require(used == (logical | bit(L'Q')), "occupancy union changed unrelated bits");
  require(choose_free_drive_letter(used, [](std::uint32_t bound) {
            require(bound == 1, "driver-only mount remained in the free set");
            return 0U;
          }) == L'Z', "driver-only mount was offered again");
  const std::array last{MountedVolume{.drive_letter = L'Z'}};
  require(choose_free_drive_letter(occupied_drive_letters(used, last)) == wchar_t{},
          "combined full occupancy still offered a letter");
  require(occupied_drive_letters(logical, {}) == logical,
          "empty driver inventory changed DOS occupancy");
  const std::array edges{MountedVolume{.drive_letter = L'A'},
                        MountedVolume{.drive_letter = L'Z'}};
  require(occupied_drive_letters(0, edges) == (bit(L'A') | bit(L'Z')),
          "boundary drive letters were not preserved");
}

void test_a_letter_in_use_is_never_handed_out() {
  // Exhaustive over single free letters: whatever is free is what comes back,
  // so no busy letter can be returned by any draw.
  for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
    const std::uint32_t used = all_busy_but({letter});
    for (int attempt = 0; attempt != 32; ++attempt) {
      require(choose_free_drive_letter(used) == letter,
              "the only free letter was not the one returned");
    }
  }
}

void test_the_system_drives_are_never_offered() {
  // Nothing is in use at all. A, B and C still must not come back.
  for (int attempt = 0; attempt != 2000; ++attempt) {
    const wchar_t letter = choose_free_drive_letter(0);
    require(letter >= L'D' && letter <= L'Z',
            "a mount was offered a letter outside D..Z");
  }
  // Even when A, B and C are the only free letters, there is nothing to give.
  require(choose_free_drive_letter(all_busy_but({L'A', L'B', L'C'})) == wchar_t{},
          "a mount was offered a reserved letter");
}

void test_a_full_machine_answers_with_nothing() {
  std::uint32_t used = 0;
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) used |= bit(letter);
  require(choose_free_drive_letter(used) == wchar_t{},
          "a machine with no free letter still produced one");
}

void test_draws_are_spread_across_the_free_letters() {
  // The point of the change: not always the same letter. With every letter
  // free, a few thousand draws must reach all 23 of them. A fixed scan order
  // would answer with one letter every time and fail here.
  std::array<int, 26> seen{};
  for (int attempt = 0; attempt != 4000; ++attempt) {
    seen[static_cast<std::size_t>(choose_free_drive_letter(0) - L'A')] += 1;
  }
  for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
    require(seen[static_cast<std::size_t>(letter - L'A')] > 0,
            "a free drive letter was never drawn");
  }
  require(seen[0] == 0 && seen[1] == 0 && seen[2] == 0,
          "a reserved letter was drawn");
}

void test_the_free_set_is_indexed_the_way_it_is_counted() {
  // The source is asked for a value under the number of free letters, and that
  // value indexes the free letters in order. Both ends are pinned here so a
  // later change cannot quietly hand back the wrong one.
  const std::uint32_t used = all_busy_but({L'E', L'Q', L'Z'});
  std::uint32_t offered_bound = 0;
  const auto fixed = [&offered_bound](const std::uint32_t bound, const std::uint32_t value) {
    offered_bound = bound;
    return value;
  };
  require(choose_free_drive_letter(used, [&](std::uint32_t b) { return fixed(b, 0); }) == L'E' &&
              offered_bound == 3,
          "the first free letter was not index zero of three");
  require(choose_free_drive_letter(used, [&](std::uint32_t b) { return fixed(b, 1); }) == L'Q',
          "the second free letter was not index one");
  require(choose_free_drive_letter(used, [&](std::uint32_t b) { return fixed(b, 2); }) == L'Z',
          "the last free letter was not index two");

  bool refused = false;
  try {
    static_cast<void>(choose_free_drive_letter(used, [](std::uint32_t) { return 3U; }));
  } catch (const std::runtime_error&) {
    refused = true;
  }
  require(refused, "a source answering past the free set produced a letter");

  bool no_source = false;
  try {
    static_cast<void>(choose_free_drive_letter(used, {}));
  } catch (const std::invalid_argument&) {
    no_source = true;
  }
  require(no_source, "a missing random source still produced a letter");
}

void test_the_random_source_stays_inside_its_bound() {
  require(uniform_random_below(0) == 0 && uniform_random_below(1) == 0,
          "a degenerate bound did not answer zero");
  for (std::uint32_t bound = 2; bound <= kMountDriveCount; ++bound) {
    for (int attempt = 0; attempt != 200; ++attempt) {
      require(uniform_random_below(bound) < bound,
              "the random source answered outside its bound");
    }
  }
}

}  // namespace

int main() {
  try {
    test_driver_only_mounts_are_excluded_from_the_free_set();
    test_a_letter_in_use_is_never_handed_out();
    test_the_system_drives_are_never_offered();
    test_a_full_machine_answers_with_nothing();
    test_draws_are_spread_across_the_free_letters();
    test_the_free_set_is_indexed_the_way_it_is_counted();
    test_the_random_source_stays_inside_its_bound();
  } catch (const std::exception& error) {
    std::cerr << "drive_letter_pool_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "drive_letter_pool_tests passed\n";
  return 0;
}
