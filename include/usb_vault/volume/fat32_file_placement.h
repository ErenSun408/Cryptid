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

// Both handles must be synchronous and exclusively used by this operation.
// Source must be opened without write/delete sharing and target must remain
// exclusively open after preparation. Progress is in bytes; callback exceptions
// stop the operation. A failed/cancelled copy leaves a caller-owned partial file.
// Success includes a flush and revalidation; it is not a power-loss guarantee.
void write_prepared_fat32_file(HANDLE source, HANDLE target, const MountIdentity& expected,
                               std::uint64_t protected_start_in_volume,
                               const Fat32AllocationPlan& plan,
                               const std::function<bool()>& cancelled = {},
                               const std::function<void(std::uint64_t)>& progress = {});
}
