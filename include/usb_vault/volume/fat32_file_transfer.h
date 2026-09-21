#pragma once
#include "usb_vault/volume/fat32_file_placement.h"
#include <exception>
#include <string_view>
namespace usb_vault::volume {
struct Fat32TransferResult final {
  std::exception_ptr failure;
  DWORD cleanup_error{};
  // Accepted deletion is not proof of persistence after a protected write.
  // Any failure stops the queue and requires session-level recovery/recheck.
  bool deletion_requested{};
};
class Fat32TransferTarget;
Fat32TransferResult transfer_fat32_file(HANDLE source, Fat32TransferTarget& target,
    const MountIdentity& expected, std::uint64_t boundary, const Fat32AllocationPlan& plan,
    const std::function<bool()>& cancelled = {},
    const std::function<void(std::uint64_t)>& progress = {}, bool retain_success = false);

// Creates exactly one NEW file with exclusive sharing and DELETE access.
// Creation must run under the requesting user's authority. This class does not
// authorize a path for an elevated caller; the helper must validate its parent.
// Unused targets are closed without deletion. Transfer or containing-tree failure
// requests deletion through this owned handle, never by reopening a path.
class Fat32TransferTarget final {
 public:
  explicit Fat32TransferTarget(std::wstring_view path);
  ~Fat32TransferTarget() noexcept;
  Fat32TransferTarget(const Fat32TransferTarget&) = delete;
  Fat32TransferTarget& operator=(const Fat32TransferTarget&) = delete;
  // Used by a containing tree transaction before releasing successful handles.
  DWORD discard() noexcept;
 private:
  friend Fat32TransferResult transfer_fat32_file(HANDLE, Fat32TransferTarget&,
      const MountIdentity&, std::uint64_t, const Fat32AllocationPlan&,
      const std::function<bool()>&, const std::function<void(std::uint64_t)>&, bool);
  DWORD close() noexcept;
  HANDLE file_{INVALID_HANDLE_VALUE};
  bool consumed_{};
};
}
