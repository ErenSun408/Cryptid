#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace usb_vault::volume {
namespace {
constexpr GUID kBasicData{0xebd0a0a2, 0xb9e5, 0x4433,
    {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
class Handle final {
 public:
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  ~Handle() { if (value_ != INVALID_HANDLE_VALUE) ::CloseHandle(value_); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const noexcept { return value_; }
 private:
  HANDLE value_;
};
class VolumeSearch final {
 public:
  explicit VolumeSearch(HANDLE value) noexcept : value_(value) {}
  ~VolumeSearch() { if (value_ != INVALID_HANDLE_VALUE) ::FindVolumeClose(value_); }
  VolumeSearch(const VolumeSearch&) = delete;
  VolumeSearch& operator=(const VolumeSearch&) = delete;
  HANDLE get() const noexcept { return value_; }
 private:
  HANDLE value_;
};
[[noreturn]] void fail(DWORD error, const char* what) {
  throw std::system_error(static_cast<int>(error), std::system_category(), what);
}
std::wstring windows_volume() {
  std::vector<wchar_t> directory(32768), root(32768);
  std::array<wchar_t, 64> volume{};
  const UINT count = ::GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
  if (!count) fail(::GetLastError(), "query Windows directory");
  if (count >= directory.size()) fail(ERROR_INSUFFICIENT_BUFFER, "Windows directory too long");
  if (!::GetVolumePathNameW(directory.data(), root.data(), static_cast<DWORD>(root.size())) ||
      !::GetVolumeNameForVolumeMountPointW(root.data(), volume.data(), static_cast<DWORD>(volume.size())))
    fail(::GetLastError(), "identify Windows volume");
  return volume.data();
}
template<class T>
bool query(HANDLE handle, DWORD code, T& value, DWORD& first_error) noexcept {
  DWORD returned{};
  if (!::DeviceIoControl(handle, code, nullptr, 0, &value,
                        static_cast<DWORD>(sizeof(T)), &returned, nullptr)) {
    const DWORD error = ::GetLastError();
    if (!first_error) first_error = error;
    return false;
  }
  if (returned < sizeof(T)) {
    if (!first_error) first_error = ERROR_INVALID_DATA;
    return false;
  }
  return true;
}
PartitionCandidate inspect(const wchar_t* path, const std::wstring& system_volume) {
  PartitionCandidate result{.volume_path = path};
  detail::PartitionObservation observed;
  const int comparison = ::CompareStringOrdinal(path, -1, system_volume.c_str(), -1, TRUE);
  if (!comparison) fail(::GetLastError(), "compare Windows volume identity");
  observed.windows_volume = comparison == CSTR_EQUAL;
  if (observed.windows_volume) {
    result.availability = PartitionAvailability::windows_volume;
    return result;
  }
  // Paths only come from FindFirst/NextVolume, never from a caller or page.
  if (!result.volume_path.starts_with(L"\\\\?\\Volume{") || result.volume_path.back() != L'\\')
    fail(ERROR_INVALID_DATA, "unexpected enumerated volume name");
  std::wstring device = result.volume_path;
  device.pop_back();
  // Zero desired access: query metadata only, including on access-limited hosts.
  const Handle handle(::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, 0, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    result.query_error = ::GetLastError();
    return result;
  }
  DWORD error{};
  observed.partition_known = query(handle.get(), IOCTL_DISK_GET_PARTITION_INFO_EX, observed.partition, error);
  observed.extent_known = query(handle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, observed.extents, error);
  observed.geometry_known = query(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, observed.geometry, error);
  observed.device_known = query(handle.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER_EX, observed.device, error);
  std::array<wchar_t, 32> filesystem{};
  DWORD filesystem_serial{};
  observed.filesystem_known = ::GetVolumeInformationW(path, nullptr, 0, &filesystem_serial, nullptr, nullptr,
      filesystem.data(), static_cast<DWORD>(filesystem.size())) != FALSE;
  if (!observed.filesystem_known && !error) error = ::GetLastError();
  result.filesystem_serial = filesystem_serial;
  observed.filesystem = filesystem.data();
  if (observed.partition_known) {
    if (observed.partition.PartitionStyle == PARTITION_STYLE_GPT) {
      result.layout = PartitionLayout::gpt;
      static_assert(sizeof(observed.partition.Gpt.PartitionId) == 16);
      std::memcpy(result.partition_id.data(), &observed.partition.Gpt.PartitionId, 16);
    } else if (observed.partition.PartitionStyle == PARTITION_STYLE_MBR) {
      result.layout = PartitionLayout::mbr;
    }
  }
  if (observed.device_known) {
    static_assert(sizeof(observed.device.DeviceGuid) == 16);
    std::memcpy(result.device_id.data(), &observed.device.DeviceGuid, 16);
    result.device_id_flags = observed.device.Flags;
  }
  result.availability = detail::assess_partition(observed);
  result.query_error = error;
  if (observed.partition_known && observed.partition.StartingOffset.QuadPart >= 0 &&
      observed.partition.PartitionLength.QuadPart > 0) {
    result.partition_number = observed.partition.PartitionNumber;
    result.offset_bytes = static_cast<std::uint64_t>(observed.partition.StartingOffset.QuadPart);
    result.length_bytes = static_cast<std::uint64_t>(observed.partition.PartitionLength.QuadPart);
  }
  if (observed.extent_known && observed.extents.NumberOfDiskExtents == 1)
    result.disk_number = observed.extents.Extents[0].DiskNumber;
  if (observed.geometry_known) result.logical_sector_bytes = observed.geometry.BytesPerSector;
  return result;
}
}  // namespace

PartitionAvailability detail::assess_partition(const PartitionObservation& value) noexcept {
  using A = PartitionAvailability;
  if (value.windows_volume) return A::windows_volume;
  if (!value.partition_known || !value.extent_known || !value.geometry_known)
    return A::metadata_unavailable;
  const auto& part = value.partition;
  if (part.PartitionStyle == PARTITION_STYLE_GPT) {
    if (!::IsEqualGUID(part.Gpt.PartitionType, kBasicData)) return A::protected_partition;
    // Only the no-default-drive-letter flag is harmless for the discovery MVP.
    if ((part.Gpt.Attributes & ~GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER) != 0)
      return A::protected_partition;
    if (::IsEqualGUID(part.Gpt.PartitionId, GUID{})) return A::metadata_unavailable;
  } else if (part.PartitionStyle == PARTITION_STYLE_MBR) {
    if (part.Mbr.BootIndicator) return A::protected_partition;
    if (!part.Mbr.RecognizedPartition || part.Mbr.PartitionType != PARTITION_IFS)
      return A::unsupported_layout;
  } else return A::unsupported_layout;
  if (value.extents.NumberOfDiskExtents != 1 || part.PartitionNumber == 0 ||
      part.StartingOffset.QuadPart <= 0 || part.PartitionLength.QuadPart <= 0 ||
      part.StartingOffset.QuadPart > (std::numeric_limits<LONGLONG>::max)() - part.PartitionLength.QuadPart)
    return A::inconsistent_extent;
  const auto& extent = value.extents.Extents[0];
  if (extent.StartingOffset.QuadPart != part.StartingOffset.QuadPart ||
      extent.ExtentLength.QuadPart != part.PartitionLength.QuadPart)
    return A::inconsistent_extent;
  // Candidate discovery only: no claim of validated 4Kn formatting/mount support.
  if (value.geometry.BytesPerSector != 512) return A::unsupported_sector_size;
  if (part.StartingOffset.QuadPart % 512 || part.PartitionLength.QuadPart % 512)
    return A::inconsistent_extent;
  if (!value.filesystem_known) return A::metadata_unavailable;
  if (value.filesystem != L"NTFS") return A::unsupported_filesystem;
  // Random GUIDs (missing hardware identity or collisions) are not stable.
  // Reject future/unknown flags and versions instead of guessing their meaning.
  const auto& device = value.device;
  if (!value.device_known || device.Version != sizeof(STORAGE_DEVICE_NUMBER_EX) ||
      device.Size != sizeof(STORAGE_DEVICE_NUMBER_EX) || device.DeviceType != FILE_DEVICE_DISK ||
      (device.Flags & ~STORAGE_DEVICE_FLAGS_PAGE_83_DEVICEGUID) != 0 ||
      ::IsEqualGUID(device.DeviceGuid, GUID{}) || device.DeviceNumber == MAXDWORD ||
      device.DeviceNumber != extent.DiskNumber || device.PartitionNumber != part.PartitionNumber)
    return A::device_identity_unavailable;
  return A::candidate;
}

bool matches_partition_snapshot(const PartitionCandidate& selected,
    const std::span<const PartitionCandidate> current) noexcept {
  if (selected.availability != PartitionAvailability::candidate || selected.query_error ||
      selected.volume_path.empty() || !selected.partition_number || !selected.offset_bytes ||
      !selected.length_bytes || selected.logical_sector_bytes != 512 ||
      (selected.layout != PartitionLayout::mbr && selected.layout != PartitionLayout::gpt) ||
      current.size() > 4096 || selected.device_id == std::array<std::uint8_t, 16>{} ||
      (selected.device_id_flags & ~STORAGE_DEVICE_FLAGS_PAGE_83_DEVICEGUID) != 0) return false;
  if (selected.layout == PartitionLayout::gpt && selected.partition_id == std::array<std::uint8_t, 16>{}) return false;
  std::size_t matching_paths{};
  bool unchanged{};
  for (const auto& item : current) {
    // Enumerator spelling is stable. Fail closed even on a case-only change.
    if (item.volume_path != selected.volume_path) continue;
    ++matching_paths;
    unchanged = item == selected;
  }
  return matching_paths == 1 && unchanged;
}

std::vector<PartitionCandidate> enumerate_partition_candidates() {
  const std::wstring system_volume = windows_volume();
  std::array<wchar_t, 64> name{};
  const VolumeSearch search(::FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size())));
  if (search.get() == INVALID_HANDLE_VALUE) fail(::GetLastError(), "enumerate volumes");
  std::vector<PartitionCandidate> result;
  do {
    if (result.size() >= 4096) fail(ERROR_INVALID_DATA, "volume inventory exceeds bound");
    result.push_back(inspect(name.data(), system_volume));
    if (::FindNextVolumeW(search.get(), name.data(), static_cast<DWORD>(name.size()))) continue;
    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES) fail(error, "continue volume enumeration");
    break;
  } while (true);
  return result;
}
}  // namespace usb_vault::volume
