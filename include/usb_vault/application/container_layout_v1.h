#pragma once

#include "usb_vault/security/kdf_v1.h"

#include <array>
#include <cstddef>

namespace usb_vault::application {

inline constexpr wchar_t kContainerAFilename[] = L"container_a.hc";
inline constexpr wchar_t kContainerBFilename[] = L"container_b.hc";

inline constexpr security::KdfV1Salt kContainerASalt{
    std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
    std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
    std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B},
    std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}};
inline constexpr security::KdfV1Salt kContainerBSalt{
    std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
    std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
    std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
    std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};

inline constexpr std::array kContainerAPasswordInput{
    std::byte{'d'}, std::byte{'i'}, std::byte{'s'}, std::byte{'k'},
    std::byte{'A'}};

}  // namespace usb_vault::application
