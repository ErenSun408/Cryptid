#pragma once
#include "usb_vault/volume/partition_inplace_plan.h"
#include "usb_vault/volume/partition_catalog.h"
#include <Windows.h>

namespace usb_vault::volume::detail {
// Borrowed readable volume handle, inspected before and after NTFS queries.
// Read-only, including the dirty check. Does not lock, shrink, seek, dismount,
// move clusters or prove shrinkability. The writer must recheck its own gates.
[[nodiscard]] PartitionInplacePlan inspect_partition_inplace(
    HANDLE handle, const PartitionCandidate& expected);
}
