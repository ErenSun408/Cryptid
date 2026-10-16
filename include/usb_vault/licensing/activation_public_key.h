#pragma once

#include "usb_vault/licensing/activation_code_v1.h"

namespace usb_vault::licensing {

// Public half of the production activation signing key (key ID 1), exported
// from the issuing tool with --export-public-key. The private half never
// enters this repository. Replacing it invalidates every issued code.
inline constexpr ActivationPublicKey kProductActivationPublicKey{
    std::byte{0x2E}, std::byte{0x64}, std::byte{0xA0}, std::byte{0xA6}, std::byte{0x77}, std::byte{0xE7}, std::byte{0x55}, std::byte{0xC2},
    std::byte{0x1F}, std::byte{0xE5}, std::byte{0x1B}, std::byte{0x1F}, std::byte{0x27}, std::byte{0x6E}, std::byte{0xA4}, std::byte{0xF3},
    std::byte{0x7D}, std::byte{0x70}, std::byte{0x44}, std::byte{0xD7}, std::byte{0xB9}, std::byte{0xA9}, std::byte{0xF2}, std::byte{0xB4},
    std::byte{0xBA}, std::byte{0xAA}, std::byte{0xB3}, std::byte{0x5A}, std::byte{0xEC}, std::byte{0xFF}, std::byte{0x59}, std::byte{0xD1},
    std::byte{0x74}, std::byte{0xDD}, std::byte{0x69}, std::byte{0xA4}, std::byte{0x7A}, std::byte{0x9E}, std::byte{0xF0}, std::byte{0x86},
    std::byte{0x13}, std::byte{0x5A}, std::byte{0x6E}, std::byte{0x49}, std::byte{0x3D}, std::byte{0xD7}, std::byte{0xB4}, std::byte{0xD4},
    std::byte{0x92}, std::byte{0xC6}, std::byte{0x1E}, std::byte{0x5C}, std::byte{0x19}, std::byte{0x44}, std::byte{0x83}, std::byte{0xB3},
    std::byte{0xB2}, std::byte{0xBD}, std::byte{0x6C}, std::byte{0x2C}, std::byte{0xC1}, std::byte{0x28}, std::byte{0x3A}, std::byte{0xBD},
};

}  // namespace usb_vault::licensing
