#pragma once

#include "usb_vault/platform/abi_checks.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace usb_vault::volume::detail::abi {

inline constexpr std::uint32_t kSupportedDriverVersion = 0x126;
inline constexpr std::size_t kMaxPath = 260;
inline constexpr std::size_t kMaxPassword = 128;
inline constexpr std::size_t kVolumeIdSize = 32;
inline constexpr std::size_t kDriveCount = 26;
inline constexpr std::size_t kLabelCharacters = 33;
inline constexpr std::int32_t kMaxPimValue = 2'147'468;

// Clean-room declarations of the externally observable VeraCrypt 1.26 driver
// ABI. Product code intentionally does not include VeraCrypt source headers.
#pragma pack(push, 1)

struct Password final {
  std::uint32_t length;
  std::uint8_t text[kMaxPassword + 1];
  std::uint8_t padding[3];
};

struct MountRequest final {
  std::int32_t return_code;
  std::int32_t filesystem_dirty;
  std::int32_t read_only_after_access_denied;
  std::int32_t read_only_after_device_write_protected;
  wchar_t volume_path[kMaxPath];
  Password volume_password;
  std::int32_t cache_password;
  std::int32_t drive_number;
  std::uint32_t bytes_per_sector;
  std::int32_t mount_read_only;
  std::int32_t mount_removable;
  std::int32_t exclusive_access;
  std::int32_t mount_manager;
  std::int32_t preserve_timestamp;
  std::int32_t partition_in_inactive_system_scope;
  std::int32_t inactive_system_scope_drive_number;
  std::int32_t system_favorite;
  std::int32_t protect_hidden_volume;
  Password protected_hidden_volume_password;
  std::int32_t use_backup_header;
  std::int32_t recovery_mode;
  std::int32_t pkcs5_prf;
  std::int32_t protected_hidden_volume_pkcs5_prf;
  std::int32_t read_only_after_partial_system_encryption;
  std::uint32_t bytes_per_physical_sector;
  std::int32_t volume_pim;
  std::int32_t protected_hidden_volume_pim;
  wchar_t label[kLabelCharacters];
  std::int32_t is_ntfs;
  std::int32_t driver_set_label;
  std::int32_t cache_pim;
  std::uint32_t maximum_transfer_length;
  std::uint32_t maximum_physical_pages;
  std::uint32_t alignment_mask;
  std::int32_t master_key_vulnerable;
};

struct UnmountRequest final {
  std::int32_t drive_number;
  std::int32_t ignore_open_files;
  std::int32_t hidden_volume_protection_triggered;
  std::int32_t return_code;
};

struct MountedVolumes final {
  std::uint32_t mounted_drives;
  wchar_t volume_path[kDriveCount][kMaxPath];
  wchar_t label[kDriveCount][kLabelCharacters];
  wchar_t volume_id[kDriveCount][kVolumeIdSize];
  std::uint64_t disk_length[kDriveCount];
  std::int32_t encryption_algorithm[kDriveCount];
  std::int32_t volume_type[kDriveCount];
  std::int32_t reserved[kDriveCount];
};

#pragma pack(pop)

static_assert(sizeof(wchar_t) == 2, "VeraCrypt ABI requires 2-byte wchar_t");

USB_VAULT_ASSERT_STANDARD_LAYOUT(Password);
USB_VAULT_ASSERT_SIZE(Password, 136);
USB_VAULT_ASSERT_OFFSET(Password, length, 0);
USB_VAULT_ASSERT_OFFSET(Password, text, 4);
USB_VAULT_ASSERT_OFFSET(Password, padding, 133);

USB_VAULT_ASSERT_STANDARD_LAYOUT(MountRequest);
USB_VAULT_ASSERT_SIZE(MountRequest, 982);
USB_VAULT_ASSERT_OFFSET(MountRequest, return_code, 0);
USB_VAULT_ASSERT_OFFSET(MountRequest, filesystem_dirty, 4);
USB_VAULT_ASSERT_OFFSET(MountRequest, read_only_after_access_denied, 8);
USB_VAULT_ASSERT_OFFSET(MountRequest,
                        read_only_after_device_write_protected, 12);
USB_VAULT_ASSERT_OFFSET(MountRequest, volume_path, 16);
USB_VAULT_ASSERT_OFFSET(MountRequest, volume_password, 536);
USB_VAULT_ASSERT_OFFSET(MountRequest, cache_password, 672);
USB_VAULT_ASSERT_OFFSET(MountRequest, drive_number, 676);
USB_VAULT_ASSERT_OFFSET(MountRequest, bytes_per_sector, 680);
USB_VAULT_ASSERT_OFFSET(MountRequest, mount_read_only, 684);
USB_VAULT_ASSERT_OFFSET(MountRequest, mount_removable, 688);
USB_VAULT_ASSERT_OFFSET(MountRequest, exclusive_access, 692);
USB_VAULT_ASSERT_OFFSET(MountRequest, mount_manager, 696);
USB_VAULT_ASSERT_OFFSET(MountRequest, preserve_timestamp, 700);
USB_VAULT_ASSERT_OFFSET(MountRequest, partition_in_inactive_system_scope, 704);
USB_VAULT_ASSERT_OFFSET(MountRequest,
                        inactive_system_scope_drive_number, 708);
USB_VAULT_ASSERT_OFFSET(MountRequest, system_favorite, 712);
USB_VAULT_ASSERT_OFFSET(MountRequest, protect_hidden_volume, 716);
USB_VAULT_ASSERT_OFFSET(MountRequest, protected_hidden_volume_password, 720);
USB_VAULT_ASSERT_OFFSET(MountRequest, use_backup_header, 856);
USB_VAULT_ASSERT_OFFSET(MountRequest, recovery_mode, 860);
USB_VAULT_ASSERT_OFFSET(MountRequest, pkcs5_prf, 864);
USB_VAULT_ASSERT_OFFSET(MountRequest,
                        protected_hidden_volume_pkcs5_prf, 868);
USB_VAULT_ASSERT_OFFSET(MountRequest,
                        read_only_after_partial_system_encryption, 872);
USB_VAULT_ASSERT_OFFSET(MountRequest, bytes_per_physical_sector, 876);
USB_VAULT_ASSERT_OFFSET(MountRequest, volume_pim, 880);
USB_VAULT_ASSERT_OFFSET(MountRequest, protected_hidden_volume_pim, 884);
USB_VAULT_ASSERT_OFFSET(MountRequest, label, 888);
USB_VAULT_ASSERT_OFFSET(MountRequest, is_ntfs, 954);
USB_VAULT_ASSERT_OFFSET(MountRequest, driver_set_label, 958);
USB_VAULT_ASSERT_OFFSET(MountRequest, cache_pim, 962);
USB_VAULT_ASSERT_OFFSET(MountRequest, maximum_transfer_length, 966);
USB_VAULT_ASSERT_OFFSET(MountRequest, maximum_physical_pages, 970);
USB_VAULT_ASSERT_OFFSET(MountRequest, alignment_mask, 974);
USB_VAULT_ASSERT_OFFSET(MountRequest, master_key_vulnerable, 978);

USB_VAULT_ASSERT_STANDARD_LAYOUT(UnmountRequest);
USB_VAULT_ASSERT_SIZE(UnmountRequest, 16);
USB_VAULT_ASSERT_OFFSET(UnmountRequest, drive_number, 0);
USB_VAULT_ASSERT_OFFSET(UnmountRequest, ignore_open_files, 4);
USB_VAULT_ASSERT_OFFSET(UnmountRequest,
                        hidden_volume_protection_triggered, 8);
USB_VAULT_ASSERT_OFFSET(UnmountRequest, return_code, 12);

USB_VAULT_ASSERT_STANDARD_LAYOUT(MountedVolumes);
USB_VAULT_ASSERT_SIZE(MountedVolumes, 17424);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, mounted_drives, 0);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, volume_path, 4);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, label, 13524);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, volume_id, 15240);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, disk_length, 16904);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, encryption_algorithm, 17112);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, volume_type, 17216);
USB_VAULT_ASSERT_OFFSET(MountedVolumes, reserved, 17320);

}  // namespace usb_vault::volume::detail::abi
