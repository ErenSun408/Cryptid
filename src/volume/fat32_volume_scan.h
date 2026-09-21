#pragma once
#include "usb_vault/volume/fat32_safe_space.h"
#include "volume/fat32_safe_space_detail.h"
namespace usb_vault::volume::detail {
Fat32Geometry read_fat32_geometry(HANDLE volume, std::uint64_t length, std::uint64_t boundary);
std::uint64_t free_fat32_prefix(HANDLE volume, Fat32Geometry geometry,
                              std::uint64_t first, std::uint64_t count);
}
