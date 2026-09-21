#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <system_error>

namespace usb_vault::volume_creation {

enum class CreationError : std::int32_t {
  invalid_request = 1,
  protocol_error = 2,
  elevation_cancelled = 3,
  helper_launch_failed = 4,
  helper_connection_failed = 5,
  helper_not_elevated = 6,
  format_backend_unavailable = 7,
  format_initialization_failed = 8,
  format_rejected = 9,
  target_exists = 10,
  target_access_denied = 11,
  mount_failed = 12,
  bitmap_scan_failed = 13,
  insufficient_tail_space = 14,
  cleanup_failed = 15,
  operation_cancelled = 16,
  io_error = 17,
  helper_terminated = 18,
};

[[nodiscard]] const std::error_category& creation_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(CreationError error) noexcept;

enum class CreationStage : std::uint32_t {
  formatting = 1,
  formatting_outer = 2,
  scanning_tail_space = 3,
  formatting_hidden = 4,
  authorized = 5,
  allocated = 6,
  copying = 7,
};

struct CreationProgress final {
  CreationStage stage{CreationStage::formatting};
  std::uint32_t percent{};
};

using ProgressCallback = std::function<void(const CreationProgress&)>;

// An empty function means the creation cannot be cancelled. It is asked on the
// thread that started the creation, once per progress report, and must not
// throw: the helper is inside the format backend when the answer is acted on,
// and it needs this pipe alive afterwards to delete the half-written container
// as the user who asked for it. Answering true is a request, not a guarantee:
// a creation that has already finished stays finished.
using CancellationQuery = std::function<bool()>;

// Launches the separately distributed volume-creation helper through the UAC
// consent path. Passwords are sent only through a local, per-invocation named
// pipe and are never placed in command-line arguments or environment variables.
class VolumeCreator final {
 public:
  explicit VolumeCreator(std::filesystem::path helper_path);

  void create_standard(std::wstring_view target_path,
                       std::uint64_t size_bytes,
                       const security::SecureBuffer& volume_password,
                       const ProgressCallback& progress = {},
                       const CancellationQuery& cancelled = {}) const;

  // Creates one self-contained outer+hidden container. requested_hidden_bytes
  // may be zero to use the largest safe tail-aligned size determined from the
  // real filesystem allocation bitmap.
  void create_with_hidden(
      std::wstring_view target_path,
      std::uint64_t outer_size_bytes,
      std::uint64_t requested_hidden_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password,
      const ProgressCallback& progress = {},
      const CancellationQuery& cancelled = {}) const;

  // Free space at the end of a mounted volume, which needs a volume handle
  // and so an elevation. The helper measures and exits; nothing stays running.
  [[nodiscard]] std::uint64_t scan_tail_space(wchar_t drive_letter) const;

 private:
  std::filesystem::path helper_path_;
};

// Dedicated upload consent, bound to one maintenance mount. A failure closes
// the pipe; another mount cannot reuse it. It accepts no measurement or creation.
class VolumeImportSession final {
 public:
  explicit VolumeImportSession(std::filesystem::path helper_path);
  ~VolumeImportSession() noexcept;
  VolumeImportSession(const VolumeImportSession&) = delete;
  VolumeImportSession& operator=(const VolumeImportSession&) = delete;
  void copy(std::wstring_view container_path, wchar_t drive, std::int32_t mount_id,
      std::uint64_t mounted_bytes, const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password, std::wstring_view source_path,
      std::wstring_view destination_relative, const ProgressCallback& progress = {},
      const CancellationQuery& cancelled = {});
  void close() noexcept;
  [[nodiscard]] bool open() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Separate one-operation consent; the measurement session never accepts writes.
// Source and target handles are opened under caller impersonation, while only
// fixed authenticated FAT32 device operations use elevation. Failure requires
// ending/rechecking the maintenance mount before further uploads.
void import_protected_file(const std::filesystem::path& helper_path,
    std::wstring_view container_path, wchar_t drive, std::int32_t mount_id,
    std::uint64_t mounted_bytes, const security::SecureBuffer& outer_password,
    const security::SecureBuffer& hidden_password, std::wstring_view source_path,
    std::wstring_view destination_relative, const ProgressCallback& progress = {},
    const CancellationQuery& cancelled = {});

// One consent for a run of measurements, so a number can be refreshed while
// the user is still writing files without asking again.
//
// The helper is launched on the first measurement, not on construction, and it
// answers nothing but measurements. Closing this ends it: the pipe going away
// is what the helper reads as the end of the session, and it also gives up on
// its own after sitting idle.
class VolumeMeasurementSession final {
 public:
  explicit VolumeMeasurementSession(std::filesystem::path helper_path);
  ~VolumeMeasurementSession() noexcept;

  VolumeMeasurementSession(const VolumeMeasurementSession&) = delete;
  VolumeMeasurementSession& operator=(const VolumeMeasurementSession&) = delete;
  VolumeMeasurementSession(VolumeMeasurementSession&&) noexcept;
  VolumeMeasurementSession& operator=(VolumeMeasurementSession&&) noexcept;

  [[nodiscard]] std::uint64_t tail_space(wchar_t drive_letter);
  // Caller supplies the identity captured when opening authenticated maintenance.
  // The helper independently authenticates headers and rechecks the live mount.
  [[nodiscard]] std::uint64_t protected_space(
      std::wstring_view container_path, wchar_t drive_letter, std::int32_t mount_id,
      std::uint64_t mounted_bytes, const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password);
  // Ends the session now rather than at destruction. Calling it twice, or
  // before anything was measured, does nothing.
  void close() noexcept;
  [[nodiscard]] bool open() const noexcept;

 private:
  [[nodiscard]] std::uint64_t measure(
      std::wstring_view path, std::uint32_t mounted_drive, std::int32_t mount_id,
      std::uint64_t mounted_bytes, const security::SecureBuffer& first,
      const security::SecureBuffer& second);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usb_vault::volume_creation

template <>
struct std::is_error_code_enum<usb_vault::volume_creation::CreationError>
    : true_type {};
