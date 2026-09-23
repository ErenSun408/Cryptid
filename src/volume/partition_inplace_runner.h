#pragma once
#include "usb_vault/volume/partition_inplace_plan.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace usb_vault::volume::detail {
struct InplaceRange final {
  std::uint64_t partition_bytes{}, start{}, length{};
};
enum class InplaceControl { proceed, pause, revoked };
enum class InplaceResult { paused, completed, revoked, failed };
// Private, synchronous worker boundary. Never expose this interface to the page.
// A production backend must hold an exclusive, identity-checked device handle,
// authenticate the backup header with the current card-derived password, and
// validate algorithm, flags and geometry before returning its range. Preparing
// NTFS and a new authenticated backup header is a separate operation.
class InplaceBackend {
 public:
  virtual ~InplaceBackend() = default;
  virtual InplaceControl control() = 0;
  virtual InplaceRange open_authenticated() = 0;
  virtual std::size_t read(std::uint64_t offset, std::span<std::byte> buffer) = 0;
  virtual void encrypt(std::uint64_t unit, std::span<std::byte> buffer) = 0;
  virtual std::size_t write(std::uint64_t offset, std::span<const std::byte> buffer) = 0;
  // Updates only the authenticated backup header; never persists plaintext keys.
  virtual void save_progress(const InplaceRange& range) = 0;
  virtual void flush() = 0;
  // Writes final volume headers only after all data and progress were flushed.
  virtual void finalize() = 0;
  // Release locks/handles and erase keys, including after partial open failure.
  virtual void close() noexcept = 0;
};
// Errors never return a resumable checkpoint. No retry after a short/failed write.
// Normal pause is observed at block boundaries; revocation before a data write
// prevents that write. Revocation after a write allows best-effort checkpointing
// but can never be reported as a normal pause. No crash-recovery guarantee.
[[nodiscard]] InplaceResult run_partition_inplace(InplaceBackend& backend) noexcept;
}  // namespace usb_vault::volume::detail
