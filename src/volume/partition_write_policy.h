#pragma once
#include "usb_vault/volume/partition_catalog.h"
#include "usb_vault/volume/partition_bitlocker.h"
#include <Windows.h>
#include <optional>
#include <functional>

namespace usb_vault::volume::detail {
// Missing facts must never be interpreted as false / fully decrypted.
struct PartitionWriteFacts final {
  std::optional<bool> boot_volume;
  std::optional<bool> system_volume;
  std::optional<bool> page_file_present;
  std::optional<std::uint32_t> conversion_status;
};
[[nodiscard]] PartitionBitLockerStatus inspect_partition_bitlocker(HANDLE handle, const PartitionCandidate& expected);
void begin_partition_bitlocker_decryption(HANDLE handle, const PartitionCandidate& expected, const std::function<bool()>& allowed);
[[nodiscard]] bool permits_partition_write(const PartitionWriteFacts& facts) noexcept;
// Read-only preflight, not a reusable write permit. The writer must run this
// against its retained target handle, then obtain an exclusive volume lock and
// revalidate identity before modifying anything. No lock/dismount/write here.
[[nodiscard]] bool check_partition_write_policy(
    HANDLE handle, const PartitionCandidate& expected) noexcept;
// Structural RAW gate only: requires a paused encrypted record, backup-header
// authentication and exclusive locking before writes. Never a format fallback.
[[nodiscard]] bool check_partition_resume_policy(HANDLE handle, const PartitionLocation& expected) noexcept;
}  // namespace usb_vault::volume::detail
