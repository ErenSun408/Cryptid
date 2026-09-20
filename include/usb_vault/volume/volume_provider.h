#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <chrono>
#include <cstdint>
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

// Names the hidden volume of a two-volume container so the driver can refuse
// writes that would reach it.
//
// The password is the one that opens the hidden header. The driver reads that
// header only to learn where the protected region begins: it does not mount
// the volume behind it and reports nothing about it back. A password that
// opens no hidden header and a container that has no hidden volume at all come
// back the same way, as a rejected mount, which is what lets the product refuse
// both in one sentence without first learning which of them it is looking at.
//
// The buffer is borrowed for the duration of the mount call. It is copied into
// the locked request buffer and nowhere else, and that buffer is wiped on every
// return path.
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
  // Absent is the shape of every mount but outer-volume maintenance, and the
  // shape a caller gets by saying nothing. Protection belongs to one mount and
  // is never written down: no local artifact records that a container was ever
  // mounted this way, because that would be a record of the container having
  // something to protect.
  std::optional<HiddenVolumeProtection> hidden_volume_protection{};
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
  std::chrono::milliseconds elapsed{};
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

  [[nodiscard]] UnmountResult unmount(
      wchar_t drive_letter,
      const UnmountOptions& options = {}) const;

  // Publishes a drive that was mounted with notify_shell=false. Higher-level
  // orchestration calls this only after every mount in a session succeeds.
  void notify_mounted(wchar_t drive_letter) const;

  [[nodiscard]] std::vector<MountedVolume> mounted_volumes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::volume

template <>
struct std::is_error_code_enum<usb_vault::volume::DriverError> : true_type {};
