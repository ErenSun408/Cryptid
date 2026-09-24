#include "volume/drive_letter_pool.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <stdexcept>

namespace usb_vault::volume {
namespace {

[[nodiscard]] std::uint32_t system_random_word() {
  std::uint32_t value{};
  if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value),
                        static_cast<ULONG>(sizeof(value)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("drive letter randomness is unavailable");
  }
  return value;
}

}  // namespace

std::uint32_t uniform_random_below(const std::uint32_t bound) {
  if (bound <= 1) return 0;
  // Rejecting the first (2^32 mod bound) values leaves a whole number of
  // blocks, so every outcome is equally likely. Taking the remainder alone
  // would favour the low letters, which is the bias this exists to avoid.
  const std::uint32_t rejected = (0U - bound) % bound;
  // At most 22 of 2^32 words are ever rejected, so one draw all but always
  // settles it. The cap is here so a generator that answers with a constant
  // cannot spin inside a mount: it gives up and says so instead.
  for (int attempt = 0; attempt != 64; ++attempt) {
    const std::uint32_t value = system_random_word();
    if (value >= rejected) return value % bound;
  }
  throw std::runtime_error("the random source did not settle on a drive letter");
}

wchar_t choose_free_drive_letter(const std::uint32_t used,
                                 const DriveLetterPick& pick) {
  if (!pick) throw std::invalid_argument("a drive letter needs a random source");
  std::array<wchar_t, kMountDriveCount> free{};
  std::uint32_t count = 0;
  for (wchar_t letter = kFirstMountDrive; letter <= kLastMountDrive; ++letter) {
    const auto index = static_cast<std::uint32_t>(letter - L'A');
    if ((used & (std::uint32_t{1} << index)) == 0) free[count++] = letter;
  }
  if (count == 0) return {};
  const std::uint32_t chosen = pick(count);
  if (chosen >= count) {
    throw std::runtime_error("the random source answered outside the free set");
  }
  return free[chosen];
}

wchar_t choose_free_drive_letter(const std::uint32_t used) {
  return choose_free_drive_letter(used, uniform_random_below);
}

}  // namespace usb_vault::volume
