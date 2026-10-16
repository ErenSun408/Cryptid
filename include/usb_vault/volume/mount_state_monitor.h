#pragma once

#include "usb_vault/volume/volume_provider.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace usb_vault::volume {

// Memory-only identity: no role, password or persistent volume identifier.
struct WatchedMount final {
  std::uint32_t container_id{};
  wchar_t drive_letter{};
  std::int32_t unique_id{};
  std::wstring volume_path;
  std::uint64_t disk_length{};
  std::uint64_t mount_revision{};
  friend bool operator==(const WatchedMount&, const WatchedMount&) = default;
};
// backing_lost: the driver still holds the volume, but the partition or the
// drive its container file lived on has gone, as when a USB medium is pulled.
enum class MountPresence { present, missing, replaced, unknown, backing_lost };
struct MountObservation final {
  std::uint32_t container_id{};
  MountPresence presence{MountPresence::unknown};
  friend bool operator==(const MountObservation&, const MountObservation&) = default;
};
struct MountStateUpdate final {
  std::uint64_t epoch{};
  std::vector<MountObservation> observations;
};

// Read-only seam. Each poll gets a complete inventory, then queries only the
// watched drives. An inventory failure differs from a successful empty list.
class IMountStateReader {
 public:
  virtual ~IMountStateReader() = default;
  virtual std::vector<MountedVolume> inventory() = 0;
  virtual MountIdentity identity(wchar_t drive) = 0;
  // False only when the backing is definitely gone; empty when undecided.
  virtual std::optional<bool> backing_present(std::wstring_view) { return std::nullopt; }
};
// Reads a driver volume path: \Device\HarddiskN\PartitionM is gone when it can
// no longer be opened, a \??\X:\ file when drive X: no longer exists at all.
// Any other answer, or any other error, is undecided.
[[nodiscard]] std::optional<bool> backing_device_present(std::wstring_view volume_path) noexcept;
[[nodiscard]] std::vector<MountObservation> inspect_mount_states(
    IMountStateReader& reader, std::span<const WatchedMount> targets);
[[nodiscard]] std::vector<MountObservation> read_driver_mount_states(
    std::span<const WatchedMount> targets);

// Owns one read-only worker, with a coalesced memory mailbox. Never invokes UI
// or session callbacks on that worker. Control methods run on the owning
// thread; the query runs only on the worker. Destruction joins an in-flight
// query (a synchronous driver call cannot be interrupted here).
class MountStateMonitor final {
 public:
  using Query = std::function<std::vector<MountObservation>(std::span<const WatchedMount>)>;
  explicit MountStateMonitor(Query query = read_driver_mount_states,
      std::chrono::milliseconds interval = std::chrono::seconds(1));
  ~MountStateMonitor();
  MountStateMonitor(const MountStateMonitor&) = delete;
  MountStateMonitor& operator=(const MountStateMonitor&) = delete;
  void watch(std::uint64_t epoch, std::vector<WatchedMount> targets);
  void request_refresh();
  [[nodiscard]] std::optional<MountStateUpdate> take_update();
 private:
  void run();
  Query query_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_{};
  bool requested_{};
  std::uint64_t revision_{};
  std::uint64_t epoch_{};
  std::vector<WatchedMount> targets_;
  std::optional<std::vector<MountObservation>> previous_;
  std::optional<MountStateUpdate> pending_;
  std::thread worker_;
};
}  // namespace usb_vault::volume
