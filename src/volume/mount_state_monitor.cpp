#include "usb_vault/volume/mount_state_monitor.h"
#include <Windows.h>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>

namespace usb_vault::volume {
namespace {
std::vector<MountObservation> unknown(std::span<const WatchedMount> targets) {
  std::vector<MountObservation> result;
  for (const auto& target : targets) result.push_back({target.container_id, MountPresence::unknown});
  return result;
}
bool valid_identity(const MountIdentity& value, wchar_t drive) {
  return value.drive_letter == drive && value.unique_id >= 0 &&
      !value.volume_path.empty() && value.disk_length != 0;
}
}  // namespace

std::vector<MountObservation> inspect_mount_states(
    IMountStateReader& reader, std::span<const WatchedMount> targets) {
  auto result = unknown(targets);
  std::vector<MountedVolume> inventory;
  try { inventory = reader.inventory(); } catch (...) { return result; }
  std::set<wchar_t> letters;
  for (const auto& mounted : inventory) {
    if (mounted.drive_letter < L'A' || mounted.drive_letter > L'Z' ||
        !letters.insert(mounted.drive_letter).second) return result;
  }
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto& target = targets[index];
    if (!letters.contains(target.drive_letter)) {
      result[index].presence = MountPresence::missing;
      continue;
    }
    try {
      const auto actual = reader.identity(target.drive_letter);
      if (!valid_identity(actual, target.drive_letter)) continue;
      if (target.unique_id < 0) continue; // No baseline: only absence can be established.
      result[index].presence = actual.unique_id == target.unique_id &&
          actual.volume_path == target.volume_path && actual.disk_length == target.disk_length
          ? MountPresence::present : MountPresence::replaced;
      if (result[index].presence == MountPresence::present) {
        const auto backing = reader.backing_present(target.volume_path);
        if (backing.has_value() && !*backing) result[index].presence = MountPresence::backing_lost;
      }
    } catch (...) {
      // A drive may vanish between inventory and properties. The next poll
      // will confirm absence; never reinterpret a query error as removal.
    }
  }
  return result;
}

std::optional<bool> backing_device_present(const std::wstring_view path) noexcept {
  try {
    if (path.starts_with(L"\\Device\\Harddisk") && path.find(L"\\Partition") != std::wstring_view::npos &&
        path.size() < 128) {
      const std::wstring device = L"\\\\?\\GLOBALROOT" + std::wstring(path);
      const HANDLE handle = ::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle != INVALID_HANDLE_VALUE) { ::CloseHandle(handle); return true; }
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NO_SUCH_DEVICE)
        return false;
      return std::nullopt;
    }
    if (path.size() > 6 && path.starts_with(L"\\??\\") && path[5] == L':' && path[6] == L'\\') {
      const wchar_t letter = path[4];
      if (letter < L'A' || letter > L'Z') return std::nullopt;
      const DWORD drives = ::GetLogicalDrives();
      if (!drives) return std::nullopt;
      if (drives & (DWORD{1} << (letter - L'A'))) return true;
      // Gone from the drive list too, not merely unmounted from a folder.
      const wchar_t name[] = {letter, L':', L'\0'};
      std::array<wchar_t, 1024> target{};
      if (::QueryDosDeviceW(name, target.data(), static_cast<DWORD>(target.size()))) return true;
      return ::GetLastError() == ERROR_FILE_NOT_FOUND ? std::optional<bool>(false) : std::nullopt;
    }
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<MountObservation> read_driver_mount_states(std::span<const WatchedMount> targets) {
  class Reader final : public IMountStateReader {
   public:
    std::vector<MountedVolume> inventory() override { return provider_.mounted_volumes(); }
    MountIdentity identity(wchar_t drive) override { return provider_.mount_identity(drive); }
    std::optional<bool> backing_present(std::wstring_view path) override { return backing_device_present(path); }
   private:
    VolumeProvider provider_;
  } reader;
  return inspect_mount_states(reader, targets);
}

MountStateMonitor::MountStateMonitor(Query query, std::chrono::milliseconds interval)
    : query_(std::move(query)), interval_(interval) {
  if (!query_ || interval_ <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("invalid mount monitor configuration");
  worker_ = std::thread([this] { run(); });
}
MountStateMonitor::~MountStateMonitor() {
  { std::lock_guard lock(mutex_); stopping_ = true; }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
}
void MountStateMonitor::watch(std::uint64_t epoch, std::vector<WatchedMount> targets) {
  std::set<std::uint32_t> ids;
  std::set<wchar_t> drives;
  if (targets.size() > 26) throw std::invalid_argument("too many watched mounts");
  for (const auto& target : targets) {
    if (!target.container_id || target.drive_letter < L'A' || target.drive_letter > L'Z' ||
        (target.unique_id < -1 || (target.unique_id >= 0 && (target.volume_path.empty() || !target.disk_length))) ||
        target.volume_path.size() > 32767 || !ids.insert(target.container_id).second ||
        !drives.insert(target.drive_letter).second)
      throw std::invalid_argument("invalid watched mount");
  }
  {
    std::lock_guard lock(mutex_);
    ++revision_; epoch_ = epoch; targets_ = std::move(targets);
    previous_.reset(); pending_.reset(); requested_ = !targets_.empty();
  }
  wake_.notify_one();
}
void MountStateMonitor::request_refresh() {
  { std::lock_guard lock(mutex_); requested_ = !targets_.empty(); }
  wake_.notify_one();
}
std::optional<MountStateUpdate> MountStateMonitor::take_update() {
  std::lock_guard lock(mutex_);
  return std::exchange(pending_, std::nullopt);
}
void MountStateMonitor::run() {
  std::unique_lock lock(mutex_);
  while (!stopping_) {
    if (targets_.empty()) {
      wake_.wait(lock, [this] { return stopping_ || !targets_.empty(); });
    } else if (!requested_) {
      wake_.wait_for(lock, interval_, [this] { return stopping_ || requested_ || targets_.empty(); });
    }
    if (stopping_) break;
    if (targets_.empty()) continue;
    const auto targets = targets_;
    const auto revision = revision_;
    const auto epoch = epoch_;
    requested_ = false;
    lock.unlock();
    std::vector<MountObservation> observations;
    try { observations = query_(targets); } catch (...) { observations = unknown(targets); }
    // Reject malformed adapter output without publishing a partial success.
    bool valid = observations.size() == targets.size();
    for (std::size_t i = 0; valid && i < observations.size(); ++i)
      valid = observations[i].container_id == targets[i].container_id &&
          (observations[i].presence == MountPresence::present || observations[i].presence == MountPresence::missing ||
           observations[i].presence == MountPresence::replaced || observations[i].presence == MountPresence::unknown ||
           observations[i].presence == MountPresence::backing_lost);
    if (!valid) observations = unknown(targets);
    lock.lock();
    if (stopping_ || revision != revision_) continue;
    if (!previous_ || *previous_ != observations) {
      previous_ = observations;
      pending_ = MountStateUpdate{epoch, std::move(observations)};
    }
  }
}
}  // namespace usb_vault::volume
