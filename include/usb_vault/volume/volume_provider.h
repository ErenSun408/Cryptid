#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/partition_identity.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace usb_vault::volume {

enum class Pkcs5Prf : std::int32_t {
  sha512 = 1,
};

enum class VolumeType : std::int32_t {
  unknown = -1,
  normal = 0,
  hidden = 1,
  outer = 2,
  outer_write_prevented = 3,
  system = 4,
};

enum class DriverError : std::int32_t {
  os_error = 1,
  out_of_memory = 2,
  authentication_failed = 3,
  damaged_volume = 4,
  driver_or_drive_not_found = 5,
  volume_busy = 6,
  incorrect_volume_size = 7,
  seek_failed = 11,
  write_failed = 12,
  volume_locked = 13,
  read_failed = 14,
  version_mismatch = 15,
  incompatible_sector_size = 20,
};

[[nodiscard]] const std::error_category& driver_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(DriverError error) noexcept;

// The password that opens the hidden header, borrowed for the mount call. The
// driver reads that header only to learn what to refuse writes to.
struct HiddenVolumeProtection final {
  const security::SecureBuffer* password{nullptr};
  Pkcs5Prf prf{Pkcs5Prf::sha512};
  std::int32_t pim{0};
};

struct MountOptions final {
  wchar_t drive_letter{};
  Pkcs5Prf prf{Pkcs5Prf::sha512};
  std::int32_t pim{0};
  bool read_only{false};
  bool notify_shell{true};
  // Absent for every mount but outer-volume maintenance. Protection belongs to
  // one mount and is never written down anywhere.
  std::optional<HiddenVolumeProtection> hidden_volume_protection{};
  // Passed to the driver; it may persist this value as the filesystem label.
  // Empty preserves the existing label.
  std::wstring label{};
  // The full name Explorer shows for the drive, written for the letter before
  // the driver mounts it; empty clears a name left there. Taken back when the
  // mount fails (volume/explorer_drive_name.h).
  std::wstring explorer_name{};
};

struct UnmountOptions final {
  bool force{false};
  bool notify_shell{true};
};

struct MountResult final {
  bool filesystem_dirty{false};
  bool read_only_after_access_denied{false};
  bool read_only_after_device_write_protected{false};
  bool is_ntfs{false};
  bool master_key_vulnerable{false};
  // What the driver reports back about the label it was handed; only traced.
  bool driver_set_label{false};
  std::chrono::milliseconds elapsed{};
  // On successful mount_file/mount_partition return, always A..Z: the drive
  // number validated before the successful VeraCrypt mount transaction.
  // The pinned driver creates and unmounts its device under that number;
  // ownership does not depend on a later inventory or shell readiness query.
  wchar_t drive_letter{};
};

struct UnmountResult final {
  bool hidden_volume_protection_triggered{false};
  std::chrono::milliseconds elapsed{};
  std::uint32_t attempts{1};
};

struct MountedVolume final {
  wchar_t drive_letter{};
  std::wstring volume_path;
  std::wstring label;
  std::uint64_t disk_length{};
  std::int32_t encryption_algorithm{};
  VolumeType type{VolumeType::unknown};
};

// Ephemeral identity of one driver mount. unique_id changes on remount, but is
// not a persistent identifier and cannot be compared across driver restarts.
struct MountIdentity final {
  wchar_t drive_letter{};
  std::int32_t unique_id{};
  std::wstring volume_path;
  std::uint64_t disk_length{};
  bool protected_outer_writable{};
  std::array<std::uint8_t, 32> header_digest{};
  friend bool operator==(const MountIdentity&, const MountIdentity&) = default;
};

// Provides the validated VeraCrypt driver boundary used by higher-level
// orchestration. It does not persist paths, mount state, volume type, or other
// metadata locally.
class VolumeProvider final {
 public:
  VolumeProvider();
  ~VolumeProvider() noexcept;

  VolumeProvider(const VolumeProvider&) = delete;
  VolumeProvider& operator=(const VolumeProvider&) = delete;

  VolumeProvider(VolumeProvider&&) noexcept;
  VolumeProvider& operator=(VolumeProvider&&) noexcept;

  [[nodiscard]] std::uint32_t driver_version() const;

  [[nodiscard]] MountResult mount_file(
      std::wstring_view volume_path,
      const security::SecureBuffer& volume_password,
      const MountOptions& options) const;

  [[nodiscard]] MountResult mount_partition(const PartitionIdentity& identity,
      const security::SecureBuffer& volume_password, const MountOptions& options) const;

  [[nodiscard]] UnmountResult unmount(
      wchar_t drive_letter,
      const UnmountOptions& options = {}) const;

  // One Shell notification transaction for a complete retry/escalation batch.
  // Operations inside the callback must use notify_shell=false. Without
  // refresh_drives the caller refreshes This PC itself, see refresh_drive_list.
  void run_unmount_batch(wchar_t drive_letter, bool notify_shell, bool refresh_drives,
                         const std::function<void()>& operation) const;

  // Publishes a drive that was mounted with notify_shell=false. Higher-level
  // orchestration calls this only after every mount in a session succeeds.
  void notify_mounted(wchar_t drive_letter) const;

  [[nodiscard]] std::vector<MountedVolume> mounted_volumes() const;

  // Read-only driver query; missing drives and malformed responses fail closed.
  [[nodiscard]] MountIdentity mount_identity(wchar_t drive_letter) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Asks Explorer to re-read the drive list once, as a batch of unmounts does
// after its last item instead of after every one.
void refresh_drive_list() noexcept;

}  // namespace usb_vault::volume

template <>
struct std::is_error_code_enum<usb_vault::volume::DriverError> : true_type {};
