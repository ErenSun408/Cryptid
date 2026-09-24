#pragma once

#include <cstdint>
#include <functional>

namespace usb_vault::volume {

// Letters a mount may take. A and B are the legacy floppy pair and C is where
// Windows conventionally lives, so a container never asks for any of them.
inline constexpr wchar_t kFirstMountDrive = L'D';
inline constexpr wchar_t kLastMountDrive = L'Z';
inline constexpr std::uint32_t kMountDriveCount =
    static_cast<std::uint32_t>(kLastMountDrive - kFirstMountDrive) + 1;

// Draws a value in [0, bound) for a caller-supplied bound. The production
// source is the system CSPRNG; tests substitute a deterministic one.
using DriveLetterPick = std::function<std::uint32_t(std::uint32_t)>;

// One free letter chosen uniformly at random, or 0 when none is free.
//
// Random rather than a fixed scan order because a machine that always lands on
// the same letter accumulates exactly the kind of drive-letter history 8.6
// requires to be indistinguishable. It changes which letter a mount takes and
// nothing else: the number of drives a session presents is unchanged, which is
// the property deniability actually rests on.
//
// `used` is a bitmask over A..Z with bit 0 for A. Letters outside D..Z are
// never returned, whether or not their bits are set.
[[nodiscard]] wchar_t choose_free_drive_letter(std::uint32_t used);

// The same selection over an explicit source. A pick that answers outside
// [0, bound) is a broken source, not a usable letter, and is refused.
[[nodiscard]] wchar_t choose_free_drive_letter(std::uint32_t used,
                                               const DriveLetterPick& pick);

// Uniform in [0, bound) from the system CSPRNG, without modulo bias. Throws if
// the generator is unavailable; returns 0 for a bound of 0 or 1.
[[nodiscard]] std::uint32_t uniform_random_below(std::uint32_t bound);

}  // namespace usb_vault::volume
