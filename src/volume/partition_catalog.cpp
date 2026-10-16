#include "usb_vault/volume/partition_catalog.h"
#include "volume/partition_catalog_detail.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <array>
#include <cstddef>
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
// A failed query leaves the volume internal: exFAT and FAT32 stay rejected and
// no partition-table identity is offered. Not a query error of the candidate.
bool query_external(const HANDLE handle) noexcept {
  STORAGE_PROPERTY_QUERY property{};
  property.PropertyId = StorageDeviceProperty;
  property.QueryType = PropertyStandardQuery;
  alignas(STORAGE_DEVICE_DESCRIPTOR) std::array<std::byte, 1024> buffer{};
  DWORD returned{};
  if (!::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &property, sizeof(property),
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
      returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return false;
  STORAGE_DEVICE_DESCRIPTOR descriptor{};
  std::memcpy(&descriptor, buffer.data(), sizeof(descriptor));
  return detail::external_bus(descriptor.BusType);
}
// Reads the partition table of the disk behind one external volume. Zero
// access: metadata only. Any failure leaves the table unknown.
void read_partition_table(detail::PartitionObservation& observed) noexcept {
  try {
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(observed.extents.Extents[0].DiskNumber);
    const Handle disk(::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (disk.get() == INVALID_HANDLE_VALUE) return;
    constexpr std::size_t kHeader = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    std::vector<std::byte> buffer(kHeader + 128 * sizeof(PARTITION_INFORMATION_EX));
    DWORD returned{};
    if (!::DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                           static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
        returned < kHeader) return;
    DRIVE_LAYOUT_INFORMATION_EX layout{};
    std::memcpy(&layout, buffer.data(), kHeader);
    observed.table_style = static_cast<PARTITION_STYLE>(layout.PartitionStyle);
    if (observed.table_style == PARTITION_STYLE_GPT) observed.disk_guid = layout.Gpt.DiskId;
    else if (observed.table_style == PARTITION_STYLE_MBR) observed.mbr_signature = layout.Mbr.Signature;
    observed.table_known = true;
  } catch (...) {
    observed.table_known = false;
  }
}
PartitionCandidate inspect_handle(const HANDLE handle, const wchar_t* discovery_path = nullptr,
    const bool location_only = false) {
  PartitionCandidate result{};
  detail::PartitionObservation observed;
  DWORD error{};
  observed.partition_known = query(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, observed.partition, error);
  observed.extent_known = query(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, observed.extents, error);
  observed.geometry_known = query(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, observed.geometry, error);
  observed.device_known = query(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER_EX, observed.device, error);
  observed.external = query_external(handle);
  if (observed.external && observed.device_known && observed.extent_known &&
      observed.extents.NumberOfDiskExtents == 1 &&
      observed.device.Flags == STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_NOHWID)
    read_partition_table(observed);
  std::array<wchar_t, 32> filesystem{};
  DWORD filesystem_serial{};
  // Unprivileged discovery retains the path query. The helper prerequisite
  // never supplies this argument and reads only its already-open handle.
  if (!location_only) {
    observed.filesystem_known = (discovery_path
        ? ::GetVolumeInformationW(discovery_path, nullptr, 0, &filesystem_serial, nullptr, nullptr,
            filesystem.data(), static_cast<DWORD>(filesystem.size()))
        : ::GetVolumeInformationByHandleW(handle, nullptr, 0, &filesystem_serial, nullptr, nullptr,
            filesystem.data(), static_cast<DWORD>(filesystem.size()))) != FALSE;
    if (!observed.filesystem_known && !error) error = ::GetLastError();
  }
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
  if (const auto identity = detail::device_identity(observed)) {
    result.device_id = identity->id;
    result.device_id_flags = identity->flags;
  } else if (observed.device_known) {
    static_assert(sizeof(observed.device.DeviceGuid) == 16);
    std::memcpy(result.device_id.data(), &observed.device.DeviceGuid, 16);
    result.device_id_flags = observed.device.Flags;
  }
  result.external = observed.external;
  result.ntfs = observed.filesystem_known && observed.filesystem == L"NTFS";
  result.availability = location_only ? detail::assess_partition_location(observed)
                                      : detail::assess_partition(observed);
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
PartitionCandidate inspect(const wchar_t* path, const std::wstring& system_volume,
    const bool location_only) {
  PartitionCandidate result{.volume_path = path};
  const int comparison = ::CompareStringOrdinal(path, -1, system_volume.c_str(), -1, TRUE);
  if (!comparison) fail(::GetLastError(), "compare Windows volume identity");
  if (comparison == CSTR_EQUAL) {
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
  auto current = inspect_handle(handle.get(), path, location_only);
  current.volume_path = std::move(result.volume_path);
  return current;
}

}  // namespace

std::wstring partition_display_name(const PartitionCandidate& candidate) {
  const auto& path = candidate.volume_path;
  if (!path.starts_with(L"\\\\?\\Volume{") || path.size() > 64 || path.back() != L'\\')
    return L"盘符未知 · 卷标不可读";
  std::vector<wchar_t> paths(32768);
  DWORD required{};
  std::wstring letters;
  const bool paths_known = ::GetVolumePathNamesForVolumeNameW(path.c_str(), paths.data(),
      static_cast<DWORD>(paths.size()), &required) != FALSE;
  if (paths_known) {
    for (std::size_t pos = 0; pos < paths.size() && paths[pos];) {
      const auto count = wcsnlen_s(paths.data() + pos, paths.size() - pos);
      if (count == paths.size() - pos) break;
      // Do not disclose directory mount paths or volume GUIDs to the page.
      if (count == 3 && paths[pos] >= L'A' && paths[pos] <= L'Z' &&
          paths[pos + 1] == L':' && paths[pos + 2] == L'\\') {
        if (!letters.empty()) letters += L" / ";
        letters.append(paths.data() + pos, 2);
      }
      pos += count + 1;
    }
  }
  if (letters.empty()) letters = paths_known ? L"无盘符" : L"盘符未知";
  std::array<wchar_t, 261> label{};
  std::wstring name = L"卷标不可读";
  if (::GetVolumeInformationW(path.c_str(), label.data(), static_cast<DWORD>(label.size()),
      nullptr, nullptr, nullptr, nullptr, 0)) {
    name = label.data();
    for (auto& ch : name) if (ch < 32 || ch == 127 || (ch >= 0x202a && ch <= 0x202e) ||
        (ch >= 0x2066 && ch <= 0x2069)) ch = L' ';
    if (name.empty()) name = L"未命名卷";
  }
  return letters + L" · " + name;
}

bool detail::external_bus(const STORAGE_BUS_TYPE bus) noexcept {
  return bus == BusTypeUsb || bus == BusTypeSd || bus == BusTypeMmc;
}

std::optional<detail::DeviceIdentity> detail::device_identity(
    const PartitionObservation& value) noexcept {
  if (!value.device_known) return std::nullopt;
  const DWORD flags = value.device.Flags;
  DeviceIdentity result;
  if ((flags & ~STORAGE_DEVICE_FLAGS_PAGE_83_DEVICEGUID) == 0) {
    if (::IsEqualGUID(value.device.DeviceGuid, GUID{})) return std::nullopt;
    std::memcpy(result.id.data(), &value.device.DeviceGuid, 16);
    result.flags = flags;
    return result;
  }
  // Only a missing hardware ID on external media falls back to the partition
  // table. A GUID conflict or an unknown flag never does.
  if (flags != STORAGE_DEVICE_FLAGS_RANDOM_DEVICEGUID_REASON_NOHWID || !value.external ||
      !value.table_known || !value.partition_known ||
      value.table_style != value.partition.PartitionStyle) return std::nullopt;
  result.flags = kPartitionTableIdentity;
  if (value.table_style == PARTITION_STYLE_GPT) {
    if (::IsEqualGUID(value.disk_guid, GUID{})) return std::nullopt;
    std::memcpy(result.id.data(), &value.disk_guid, 16);
  } else if (value.table_style == PARTITION_STYLE_MBR) {
    if (value.mbr_signature == 0) return std::nullopt;
    std::memcpy(result.id.data(), &value.mbr_signature, sizeof(value.mbr_signature));
  } else {
    return std::nullopt;
  }
  return result;
}

static bool external_mbr_type(const BYTE type) noexcept {
  return type == PARTITION_FAT32 || type == PARTITION_FAT32_XINT13;
}

static PartitionAvailability assess_observation(const detail::PartitionObservation& value,
    const bool require_filesystem) noexcept {
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
    // External FAT32 media keep their FAT32 type after being encrypted, since
    // erasing never rewrites the partition table.
    if (!part.Mbr.RecognizedPartition || (part.Mbr.PartitionType != PARTITION_IFS &&
        !(value.external && external_mbr_type(part.Mbr.PartitionType))))
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
  if (require_filesystem) {
    if (!value.filesystem_known) return A::metadata_unavailable;
    // External media may be erased whatever they hold among these; only NTFS
    // can also be encrypted in place, which its own plan checks.
    const bool external_filesystem = value.external &&
        (value.filesystem == L"exFAT" || value.filesystem == L"FAT32");
    if (value.filesystem != L"NTFS" && !external_filesystem) return A::unsupported_filesystem;
  }
  // Random GUIDs (missing hardware identity or collisions) are not stable.
  // Reject future/unknown flags and versions instead of guessing their meaning.
  const auto& device = value.device;
  if (!value.device_known || device.Version != sizeof(STORAGE_DEVICE_NUMBER_EX) ||
      device.Size != sizeof(STORAGE_DEVICE_NUMBER_EX) || device.DeviceType != FILE_DEVICE_DISK ||
      !detail::device_identity(value) || device.DeviceNumber == MAXDWORD ||
      device.DeviceNumber != extent.DiskNumber || device.PartitionNumber != part.PartitionNumber)
    return A::device_identity_unavailable;
  return A::candidate;
}

PartitionAvailability detail::assess_partition(const PartitionObservation& value) noexcept {
  return assess_observation(value, true);
}

PartitionAvailability detail::assess_partition_location(const PartitionObservation& value) noexcept {
  const auto result = assess_observation(value, false);
  if (result != PartitionAvailability::candidate) return result;
  const auto length = value.partition.PartitionLength.QuadPart;
  if (length < 20LL * 1024 * 1024 || length > (1LL << 40))
    return PartitionAvailability::inconsistent_extent;
  return PartitionAvailability::candidate;
}

bool detail::matches_partition_handle_identity(const HANDLE handle,
    const PartitionCandidate& expected) noexcept {
  if (!handle || handle == INVALID_HANDLE_VALUE ||
      !matches_partition_snapshot(expected, {&expected, 1})) return false;
  try {
    auto current = inspect_handle(handle);
    // A kernel handle, not a reopened path, supplies every comparison fact.
    // The discovery path is only a label; it cannot establish handle identity.
    auto facts = expected;
    facts.volume_path.clear();
    return current == facts;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("volume/partition_catalog", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return false;
  }
}

bool matches_partition_snapshot(const PartitionCandidate& selected,
    const std::span<const PartitionCandidate> current) noexcept {
  if (selected.availability != PartitionAvailability::candidate || selected.query_error ||
      selected.volume_path.empty() || !selected.partition_number || !selected.offset_bytes ||
      !selected.length_bytes || selected.logical_sector_bytes != 512 ||
      (selected.layout != PartitionLayout::mbr && selected.layout != PartitionLayout::gpt) ||
      current.size() > 4096 || selected.device_id == std::array<std::uint8_t, 16>{} ||
      ((selected.device_id_flags & ~STORAGE_DEVICE_FLAGS_PAGE_83_DEVICEGUID) != 0 &&
       selected.device_id_flags != kPartitionTableIdentity)) return false;
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

static std::vector<PartitionCandidate> enumerate_partitions_unlogged(const bool location_only);

static std::vector<PartitionCandidate> enumerate_partitions(const bool location_only) {
  try {
    auto result = enumerate_partitions_unlogged(location_only);
    USB_VAULT_TRACE("分区", location_only ? "枚举分区位置" : "枚举分区候选",
                    "卷数 " + std::to_string(result.size()));
    for (const auto& candidate : result) {
      USB_VAULT_TRACE("分区", "分区",
                      "卷路径 " + diagnostics::utf8(candidate.volume_path) + "；磁盘 " +
                          std::to_string(candidate.disk_number) + "；分区 " +
                          std::to_string(candidate.partition_number) + "；偏移 " +
                          std::to_string(candidate.offset_bytes) + "；长度 " +
                          std::to_string(candidate.length_bytes) + "；扇区 " +
                          std::to_string(candidate.logical_sector_bytes) + "；布局 " +
                          std::to_string(static_cast<int>(candidate.layout)) + "；可用性 " +
                          std::to_string(static_cast<int>(candidate.availability)) +
                          "；查询错误 " + std::to_string(candidate.query_error));
    }
    return result;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("分区", "枚举分区失败", "");
    throw;
  }
}

static std::vector<PartitionCandidate> enumerate_partitions_unlogged(const bool location_only) {
  const std::wstring system_volume = windows_volume();
  std::array<wchar_t, 64> name{};
  const VolumeSearch search(::FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size())));
  if (search.get() == INVALID_HANDLE_VALUE) fail(::GetLastError(), "enumerate volumes");
  std::vector<PartitionCandidate> result;
  do {
    if (result.size() >= 4096) fail(ERROR_INVALID_DATA, "volume inventory exceeds bound");
    result.push_back(inspect(name.data(), system_volume, location_only));
    if (::FindNextVolumeW(search.get(), name.data(), static_cast<DWORD>(name.size()))) continue;
    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES) fail(error, "continue volume enumeration");
    break;
  } while (true);
  return result;
}
std::vector<PartitionCandidate> enumerate_partition_candidates() {
  return enumerate_partitions(false);
}

namespace {
bool valid_location_path(const std::wstring_view path) noexcept {
  if (path.size() != 49 || !path.starts_with(L"\\\\?\\Volume{") ||
      !path.ends_with(L"}\\")) return false;
  for (std::size_t i = 11; i < 47; ++i) {
    const auto c = path[i];
    if (i == 19 || i == 24 || i == 29 || i == 34) {
      if (c != L'-') return false;
    } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                 (c >= L'A' && c <= L'F'))) return false;
  }
  return true;
}
PartitionLocation location_from(const PartitionCandidate& candidate) {
  return {{candidate.device_id, candidate.device_id_flags,
      static_cast<std::uint32_t>(candidate.layout),
      candidate.logical_sector_bytes, candidate.offset_bytes, candidate.length_bytes, candidate.partition_id},
      candidate.volume_path, candidate.disk_number, candidate.partition_number};
}
bool valid_location(const PartitionLocation& location) noexcept {
  return valid_partition_identity(location.identity) &&
      valid_location_path(location.volume_path) && location.disk_number != MAXDWORD &&
      location.partition_number != 0;
}
}

std::vector<PartitionLocation> enumerate_partition_locations() {
  std::vector<PartitionLocation> result;
  for (const auto& candidate : enumerate_partitions(true)) {
    if (candidate.availability != PartitionAvailability::candidate || candidate.query_error) continue;
    auto location = location_from(candidate);
    if (valid_location(location)) result.push_back(std::move(location));
  }
  return result;
}

std::optional<PartitionLocation> resolve_partition_location(
    const PartitionIdentity& saved, const std::span<const PartitionLocation> current) {
  if (!valid_partition_identity(saved) || current.size() > 4096) return std::nullopt;
  const PartitionLocation* match = nullptr;
  for (const auto& location : current) {
    if (!valid_location(location)) return std::nullopt;
    if (location.identity != saved) continue;
    if (match) return std::nullopt;
    match = &location;
  }
  if (!match) return std::nullopt;
  for (const auto& location : current) {
    if (&location == match) continue;
    const int comparison = ::CompareStringOrdinal(location.volume_path.c_str(), -1,
        match->volume_path.c_str(), -1, TRUE);
    if (!comparison || comparison == CSTR_EQUAL) return std::nullopt;
  }
  return *match;
}

bool detail::matches_partition_location_handle(const HANDLE handle,
    const PartitionLocation& expected) noexcept {
  if (!handle || handle == INVALID_HANDLE_VALUE || !valid_location(expected)) return false;
  try {
    const auto current = inspect_handle(handle, nullptr, true);
    if (current.availability != PartitionAvailability::candidate || current.query_error) return false;
    auto actual = location_from(current);
    // The path is a discovery label, never evidence supplied by this handle.
    actual.volume_path = expected.volume_path;
    return actual == expected;
  } catch (...) { USB_VAULT_TRACE_FAILURE("volume/partition_catalog", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return false; }
}
}  // namespace usb_vault::volume
