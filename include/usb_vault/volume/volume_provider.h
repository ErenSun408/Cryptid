#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <chrono>
#include <cstdint>
#include <memory>
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

struct MountOptions final {
  wchar_t drive_letter{};
  Pkcs5Prf prf{Pkcs5Prf::sha512};
  std::int32_t pim{0};
  bool read_only{false};
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

  [[nodiscard]] UnmountResult unmount(wchar_t drive_letter,
                                      bool force = false) const;

  [[nodiscard]] std::vector<MountedVolume> mounted_volumes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::volume

template <>
struct std::is_error_code_enum<usb_vault::volume::DriverError> : true_type {};
