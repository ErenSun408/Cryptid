#include "usb_vault/volume/volume_provider.h"

#include "volume/locked_driver_buffer.h"
#include "volume/partition_catalog_detail.h"
#include "volume/mount_request.h"
#include "volume/mount_identity.h"
#include "volume/veracrypt_driver_abi.h"

#include <Windows.h>
#include <ShlObj_core.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace usb_vault::volume {
namespace {

constexpr wchar_t kDriverPath[] = L"\\\\.\\VeraCrypt";

[[nodiscard]] constexpr DWORD driver_ioctl(const DWORD code) noexcept {
  return CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800U + code, METHOD_BUFFERED,
                  FILE_ANY_ACCESS);
}

constexpr DWORD kGetDriverVersion = driver_ioctl(1);
constexpr DWORD kMountVolume = driver_ioctl(3);
constexpr DWORD kUnmountVolume = driver_ioctl(4);
constexpr DWORD kGetMountedVolumes = driver_ioctl(6);
constexpr DWORD kGetVolumeProperties = driver_ioctl(7);

class DriverErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override {
    return "veracrypt-driver";
  }

  [[nodiscard]] std::string message(const int value) const override {
    switch (value) {
      case 0:
        return "success";
      case 1:
        return "operating system error";
      case 2:
        return "out of memory";
      case 3:
        return "wrong password, wrong PIM, or unsupported volume";
      case 4:
        return "damaged volume format";
      case 5:
        return "driver or drive not found";
      case 6:
        return "volume still has open files";
      case 7:
        return "incorrect volume size";
      case 11:
        return "volume seek failed";
      case 12:
        return "volume write failed";
      case 13:
        return "volume is locked";
      case 14:
        return "volume read failed";
      case 15:
        return "driver version mismatch";
      case 20:
        return "incompatible sector size";
      default:
        return "unknown VeraCrypt driver error";
    }
  }
};

[[noreturn]] void throw_windows_error(const DWORD error,
                                      const char* const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

[[noreturn]] void throw_driver_error(const std::int32_t error,
                                     const char* const operation) {
  throw std::system_error(error, driver_error_category(), operation);
}

class DriverHandle final {
 public:
  explicit DriverHandle(const HANDLE handle) noexcept : handle_(handle) {}

  ~DriverHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(handle_));
    }
  }

  DriverHandle(const DriverHandle&) = delete;
  DriverHandle& operator=(const DriverHandle&) = delete;

  DriverHandle(DriverHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}

  DriverHandle& operator=(DriverHandle&&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[nodiscard]] DriverHandle open_driver() {
  const HANDLE handle =
      ::CreateFileW(kDriverPath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw_windows_error(::GetLastError(), "open VeraCrypt driver");
  }
  return DriverHandle(handle);
}

[[nodiscard]] std::uint32_t query_driver_version(const HANDLE handle) {
  std::uint32_t version = 0;
  DWORD bytes_returned = 0;
  if (::DeviceIoControl(handle, kGetDriverVersion, nullptr, 0, &version,
                        static_cast<DWORD>(sizeof(version)), &bytes_returned,
                        nullptr) == FALSE) {
    throw_windows_error(::GetLastError(), "query VeraCrypt driver version");
  }
  if (bytes_returned != sizeof(version)) {
    throw std::system_error(std::make_error_code(std::errc::protocol_error),
                            "invalid VeraCrypt driver version response");
  }
  return version;
}

void require_supported_version(const HANDLE handle) {
  if (query_driver_version(handle) != detail::abi::kSupportedDriverVersion) {
    throw std::system_error(
        std::make_error_code(std::errc::protocol_not_supported),
        "unsupported VeraCrypt driver ABI version");
  }
}

template <typename T>
void transact(const HANDLE handle, const DWORD ioctl, T& buffer,
              const char* const operation) {
  static_assert(sizeof(T) <= MAXDWORD);
  DWORD bytes_returned = 0;
  if (::DeviceIoControl(handle, ioctl, &buffer,
                        static_cast<DWORD>(sizeof(buffer)), &buffer,
                        static_cast<DWORD>(sizeof(buffer)), &bytes_returned,
                        nullptr) == FALSE) {
    throw_windows_error(::GetLastError(), operation);
  }
  if (bytes_returned != sizeof(buffer)) {
    throw std::system_error(std::make_error_code(std::errc::protocol_error),
                            "incomplete VeraCrypt driver response");
  }
}

void notify_shell(const LONG event, const std::int32_t drive_number) noexcept {
  const wchar_t root[] = {
      static_cast<wchar_t>(L'A' + drive_number), L':', L'\\', L'\0'};
  ::SHChangeNotify(event, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, nullptr);
}

[[nodiscard]] VolumeType to_volume_type(const std::int32_t value) noexcept {
  switch (value) {
    case 0:
      return VolumeType::normal;
    case 1:
      return VolumeType::hidden;
    case 2:
      return VolumeType::outer;
    case 3:
      return VolumeType::outer_write_prevented;
    case 4:
      return VolumeType::system;
    default:
      return VolumeType::unknown;
  }
}

template <std::size_t Size>
[[nodiscard]] std::wstring bounded_string(const wchar_t (&value)[Size]) {
  const auto end = std::find(std::begin(value), std::end(value), L'\0');
  return {std::begin(value), end};
}

template <typename T>
void require_active(const std::unique_ptr<T>& impl) {
  if (impl == nullptr) {
    throw std::logic_error("VolumeProvider was moved from");
  }
}

}  // namespace

struct VolumeProvider::Impl final {};

const std::error_category& driver_error_category() noexcept {
  static DriverErrorCategory category;
  return category;
}

std::error_code make_error_code(const DriverError error) noexcept {
  return {static_cast<int>(error), driver_error_category()};
}

VolumeProvider::VolumeProvider() : impl_(std::make_unique<Impl>()) {}

VolumeProvider::~VolumeProvider() noexcept = default;

VolumeProvider::VolumeProvider(VolumeProvider&&) noexcept = default;

VolumeProvider& VolumeProvider::operator=(VolumeProvider&&) noexcept = default;

std::uint32_t VolumeProvider::driver_version() const {
  require_active(impl_);
  const DriverHandle driver = open_driver();
  return query_driver_version(driver.get());
}

MountResult VolumeProvider::mount_file(
    const std::wstring_view volume_path,
    const security::SecureBuffer& volume_password,
    const MountOptions& options) const {
  require_active(impl_);
  detail::LockedDriverBuffer<detail::abi::MountRequest> locked_request;
  detail::abi::MountRequest& request = locked_request.get();
  detail::populate_mount_request(request, volume_path, volume_password, options);

  const auto start = std::chrono::steady_clock::now();
  const DriverHandle driver = open_driver();
  require_supported_version(driver.get());
  transact(driver.get(), kMountVolume, request, "mount VeraCrypt volume");

  const std::int32_t return_code = request.return_code;
  if (return_code != 0) {
    locked_request.clear();
    throw_driver_error(return_code, "VeraCrypt mount rejected");
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  const MountResult result{
      .filesystem_dirty = request.filesystem_dirty != 0,
      .read_only_after_access_denied =
          request.read_only_after_access_denied != 0,
      .read_only_after_device_write_protected =
          request.read_only_after_device_write_protected != 0,
      .is_ntfs = request.is_ntfs != 0,
      .master_key_vulnerable = request.master_key_vulnerable != 0,
      .elapsed = elapsed,
  };
  const std::int32_t drive_number = request.drive_number;
  locked_request.clear();

  if (options.notify_shell) {
    notify_shell(SHCNE_DRIVEADD, drive_number);
  }
  return result;
}

MountResult VolumeProvider::mount_partition(
    const PartitionIdentity& identity,
    const security::SecureBuffer& volume_password,
    const MountOptions& options) const {
  require_active(impl_);
  if (!valid_partition_identity(identity) || volume_password.size() != 64 || (options.hidden_volume_protection &&
      (!options.hidden_volume_protection->password || options.hidden_volume_protection->password->size() != 64)))
    throw std::invalid_argument("invalid partition mount request");
  const auto located = resolve_partition_location(identity, enumerate_partition_locations());
  if (!located) throw std::runtime_error("partition is absent or ambiguous");
  auto path = located->volume_path; path.pop_back();
  const DriverHandle retained(::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr));
  if (retained.get() == INVALID_HANDLE_VALUE ||
      !detail::matches_partition_location_handle(retained.get(), *located))
    throw std::runtime_error("partition identity changed before mount");
  const std::wstring native = L"\\Device\\Harddisk" + std::to_wstring(located->disk_number) +
      L"\\Partition" + std::to_wstring(located->partition_number);
  if (resolve_partition_location(identity, enumerate_partition_locations()) != located)
    throw std::runtime_error("partition location changed before mount");
  detail::LockedDriverBuffer<detail::abi::MountRequest> locked_request;
  detail::abi::MountRequest& request = locked_request.get();
  detail::populate_mount_request(request, located->volume_path, volume_password, options);
  std::fill(std::begin(request.volume_path), std::end(request.volume_path), L'\0');
  if (native.size() >= std::size(request.volume_path)) throw std::invalid_argument("partition path too long");
  std::ranges::copy(native, request.volume_path);
  request.preserve_timestamp = 0;

  const auto start = std::chrono::steady_clock::now();
  const DriverHandle driver = open_driver();
  require_supported_version(driver.get());
  transact(driver.get(), kMountVolume, request, "mount VeraCrypt volume");

  const std::int32_t return_code = request.return_code;
  if (return_code != 0) {
    locked_request.clear();
    throw_driver_error(return_code, "VeraCrypt mount rejected");
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  const MountResult result{
      .filesystem_dirty = request.filesystem_dirty != 0,
      .read_only_after_access_denied =
          request.read_only_after_access_denied != 0,
      .read_only_after_device_write_protected =
          request.read_only_after_device_write_protected != 0,
      .is_ntfs = request.is_ntfs != 0,
      .master_key_vulnerable = request.master_key_vulnerable != 0,
      .elapsed = elapsed,
  };
  const std::int32_t drive_number = request.drive_number;
  locked_request.clear();

  if (options.notify_shell) {
    notify_shell(SHCNE_DRIVEADD, drive_number);
  }
  return result;
}

UnmountResult VolumeProvider::unmount(
    const wchar_t drive_letter,
    const UnmountOptions& options) const {
  require_active(impl_);
  detail::LockedDriverBuffer<detail::abi::UnmountRequest> locked_request;
  detail::abi::UnmountRequest& request = locked_request.get();
  request.drive_number = detail::drive_number(drive_letter);
  request.ignore_open_files = options.force ? 1 : 0;

  const auto start = std::chrono::steady_clock::now();
  const DriverHandle driver = open_driver();
  require_supported_version(driver.get());
  transact(driver.get(), kUnmountVolume, request, "unmount VeraCrypt volume");

  if (request.return_code != 0) {
    throw_driver_error(request.return_code, "VeraCrypt unmount rejected");
  }

  const UnmountResult result{
      .hidden_volume_protection_triggered =
          request.hidden_volume_protection_triggered != 0,
      .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start),
      .attempts = 1,
  };
  if (options.notify_shell) {
    notify_shell(SHCNE_DRIVEREMOVED, request.drive_number);
  }
  return result;
}

void VolumeProvider::notify_mounted(const wchar_t drive_letter) const {
  require_active(impl_);
  notify_shell(SHCNE_DRIVEADD, detail::drive_number(drive_letter));
}

std::vector<MountedVolume> VolumeProvider::mounted_volumes() const {
  require_active(impl_);
  detail::LockedDriverBuffer<detail::abi::MountedVolumes> locked_volumes;
  detail::abi::MountedVolumes& volumes = locked_volumes.get();

  const DriverHandle driver = open_driver();
  require_supported_version(driver.get());
  transact(driver.get(), kGetMountedVolumes, volumes,
           "query mounted VeraCrypt volumes");

  std::vector<MountedVolume> result;
  result.reserve(detail::abi::kDriveCount);
  for (std::size_t index = 0; index < detail::abi::kDriveCount; ++index) {
    const auto mask = std::uint32_t{1} << static_cast<std::uint32_t>(index);
    if ((volumes.mounted_drives & mask) == 0) {
      continue;
    }

    result.push_back({
        .drive_letter = static_cast<wchar_t>(L'A' + index),
        .volume_path = bounded_string(volumes.volume_path[index]),
        .label = bounded_string(volumes.label[index]),
        .disk_length = volumes.disk_length[index],
        .encryption_algorithm = volumes.encryption_algorithm[index],
        .type = to_volume_type(volumes.volume_type[index]),
    });
  }
  return result;
}

MountIdentity VolumeProvider::mount_identity(const wchar_t drive_letter) const {
  require_active(impl_);
  const auto drive = detail::drive_number(drive_letter);
  detail::LockedDriverBuffer<detail::abi::VolumeProperties> locked;
  auto& properties = locked.get();
  properties.drive_number = drive;
  const auto driver = open_driver();
  require_supported_version(driver.get());
  transact(driver.get(), kGetVolumeProperties, properties,
           "query VeraCrypt mount identity");
  return detail::decode_mount_identity(properties, drive);
}

}  // namespace usb_vault::volume
