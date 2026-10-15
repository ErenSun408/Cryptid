#pragma once

#include "usb_vault/backup/export_manifest_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace usb_vault::backup {

// Phase 1 is the read-only verification, phase 2 the write; each reports the
// file bytes done and in total.
using ImportProgress =
    std::function<void(unsigned int, std::uint64_t, std::uint64_t)>;

// One package being imported. It is opened so that no other program can write
// to it, and stays that way until this object goes, so the bytes verified are
// the bytes later written.
//
// An import is two passes. verify() authenticates the whole package and
// checks its manifest without creating or writing anything; only after it has
// succeeded may write_into() run, which reads the package again, checks every
// chunk a second time, and restores the entry under a random hidden staging
// name before renaming it into place without replacing anything. When the
// write stops -- damage, a full volume, cancellation -- the staging entry is
// deleted and the destination is as it was. If the volume itself is gone,
// as after a removed card, the hidden staging entry cannot be reached and
// stays inside the encrypted volume, where nothing outside it can see it.
class ImportPackageV1 final {
 public:
  // Throws std::system_error for a file that cannot be opened for reading
  // while denying writers, and std::runtime_error for a directory. A link is
  // followed: a package the user picked may be a cloud placeholder.
  explicit ImportPackageV1(std::wstring_view absolute_package);
  ~ImportPackageV1() noexcept;
  ImportPackageV1(const ImportPackageV1&) = delete;
  ImportPackageV1& operator=(const ImportPackageV1&) = delete;

  // The read-only pass. Throws ExportPackageRejected for a wrong password or a
  // damaged package, and std::system_error with ERROR_CANCELLED when asked to
  // stop.
  const ExportManifest& verify(const security::SecureBuffer& password_utf8,
                               const std::function<bool()>& cancelled,
                               const ImportProgress& progress);

  // Room a destination with `cluster_bytes` clusters needs: every file
  // rounded up to whole clusters, plus a conservative allowance per entry for
  // file system metadata. Valid after verify().
  [[nodiscard]] std::uint64_t required_bytes(std::uint64_t cluster_bytes = 65536) const;

  // The write pass, into `relative_directory` of the mounted volume on
  // `drive_letter`. Returns the restored top-level name. Throws
  // std::system_error with ERROR_FILE_EXISTS when that name is taken,
  // ERROR_DISK_FULL when the data cannot fit, ERROR_CANCELLED when asked to
  // stop, std::invalid_argument when a restored path would be too long there,
  // and ExportPackageRejected when the second reading disagrees with the first.
  std::wstring write_into(const security::SecureBuffer& password_utf8,
                          wchar_t drive_letter, std::wstring_view relative_directory,
                          const std::function<bool()>& cancelled,
                          const ImportProgress& progress);

 private:
  void* handle_;
  std::uint64_t size_{};
  bool verified_{false};
  ExportManifest manifest_;
};

}  // namespace usb_vault::backup
