#pragma once
#include "usb_vault/volume/fat32_safe_space.h"
#include "usb_vault/volume/volume_provider.h"
#include <functional>
namespace usb_vault::volume {
// Caller must own a newly CREATE_NEW-created, empty, exclusively opened file.
// Boundary must come from authenticated geometry for this exact live mount.
// No payload is written. Failure leaves the caller responsible for its partial
// file: preserve the original error and do not assume deletion is durable.
// This is an in-process primitive, not an IPC authorization boundary.
void prepare_fat32_file(HANDLE file, const MountIdentity& expected,
                        std::uint64_t protected_start_in_volume,
                        const Fat32AllocationPlan& plan,
                        const std::function<bool()>& cancelled = {});
}
