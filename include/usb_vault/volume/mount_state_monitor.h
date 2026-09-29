#pragma once

#include "usb_vault/volume/volume_provider.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
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
};
enum class MountPresence { present, missing, replaced, unknown };
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
};
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
