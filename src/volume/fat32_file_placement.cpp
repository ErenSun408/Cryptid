#include "usb_vault/volume/fat32_file_placement.h"
#include "usb_vault/volume/fat32_file_allocation.h"
#include "volume/fat32_volume_scan.h"
#include "volume/fat32_allocation_plan_detail.h"
#include <winioctl.h>
#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>
namespace usb_vault::volume {
namespace {
struct OwnedHandle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
  OwnedHandle() = default;
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
};
void win_check(BOOL ok, const char* operation) {
  if (!ok) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), operation);
}
struct PlacementContext final {
  const MountIdentity& expected;
  const std::function<bool()>& cancelled;
  VolumeProvider provider;
  OwnedHandle volume;
  detail::Fat32Geometry geometry;
  PlacementContext(HANDLE file, const MountIdentity& identity, std::uint64_t boundary,
                   const Fat32AllocationPlan& plan, const std::function<bool()>& stop)
      : expected(identity), cancelled(stop) {
    using detail::require_geometry;
    require_geometry(expected.drive_letter >= L'A' && expected.drive_letter <= L'Z' && expected.protected_outer_writable);
    guard();
    const auto device = std::wstring(L"\\Device\\VeraCryptVolume") + expected.drive_letter;
    std::vector<wchar_t> name(32768);
    const auto length = ::GetFinalPathNameByHandleW(file, name.data(), static_cast<DWORD>(name.size()), VOLUME_NAME_NT);
    win_check(length != 0, "resolve FAT32 file device");
    require_geometry(length < name.size());
    const auto prefix = device + L"\\";
    require_geometry(length > prefix.size() && ::CompareStringOrdinal(name.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL);
    BY_HANDLE_FILE_INFORMATION attributes{};
    win_check(::GetFileInformationByHandle(file, &attributes), "read FAT32 target attributes");
    require_geometry((attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0);
    const auto raw = std::wstring(LR"(\\?\GLOBALROOT)") + device;
    volume.value = ::CreateFileW(raw.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    win_check(volume.value != INVALID_HANDLE_VALUE, "open FAT32 placement volume");
    geometry = detail::read_fat32_geometry(volume.value, expected.disk_length, boundary);
    require_geometry(plan.cluster_size == geometry.cluster_size && plan.file_bytes <= std::numeric_limits<std::uint32_t>::max());
    const auto clusters = plan.file_bytes / geometry.cluster_size + (plan.file_bytes % geometry.cluster_size != 0);
    require_geometry(plan.allocation_bytes == clusters * geometry.cluster_size && plan.runs.size() <= detail::kMaxAllocationPlanRuns);
    std::uint64_t total = 0, previous = 0;
    for (const auto& run : plan.runs) {
      require_geometry(run.cluster_count > 0 && run.first_cluster >= previous && run.first_cluster < geometry.safe_clusters && run.cluster_count <= geometry.safe_clusters - run.first_cluster);
      require_geometry(total <= clusters && run.cluster_count <= clusters - total);
      total += run.cluster_count;
      previous = run.first_cluster + run.cluster_count;
    }
    require_geometry(total == clusters);
  }
  void guard() const {
    if (cancelled && cancelled()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    detail::require_geometry(provider.mount_identity(expected.drive_letter) == expected);
  }
  void verify(HANDLE file, const Fat32AllocationPlan& plan) const {
    detail::require_geometry(read_fat32_file_allocation(file, plan.file_bytes, geometry.cluster_size, geometry.cluster_count) == plan.runs);
    guard();
  }
};
} // namespace
void prepare_fat32_file(HANDLE file, const MountIdentity& expected,
                        std::uint64_t boundary, const Fat32AllocationPlan& plan,
                        const std::function<bool()>& cancelled) {
  using detail::require_geometry;
  PlacementContext context(file, expected, boundary, plan, cancelled);
  const auto& geometry = context.geometry;
  static_cast<void>(read_fat32_file_allocation(file, 0, geometry.cluster_size, geometry.cluster_count));
  context.guard();
  LARGE_INTEGER eof{}; eof.QuadPart = static_cast<LONGLONG>(plan.file_bytes);
  win_check(::SetFilePointerEx(file, eof, nullptr, FILE_BEGIN), "seek FAT32 allocation end");
  win_check(::SetEndOfFile(file), "allocate FAT32 file");
  context.guard();
  std::uint64_t vcn = 0;
  for (const auto& destination : plan.runs) {
    auto first = destination.first_cluster;
    const auto end = first + destination.cluster_count;
    while (first < end) {
      context.guard();
      const auto current = read_fat32_file_allocation(file, plan.file_bytes, geometry.cluster_size, geometry.cluster_count);
      std::uint64_t base = 0, source = 0, remaining = 0;
      for (const auto& run : current) {
        if (vcn < base + run.cluster_count) { source = run.first_cluster + vcn - base; remaining = base + run.cluster_count - vcn; break; }
        base += run.cluster_count;
      }
      require_geometry(remaining > 0);
      auto count = std::min(end - first, remaining);
      if (source != first) {
        count = detail::free_fat32_prefix(context.volume.value, geometry, first, count);
        if (count == 0) throw std::system_error(std::make_error_code(std::errc::device_or_resource_busy), "FAT32 destination occupied");
        context.guard();
        MOVE_FILE_DATA move{};
        move.FileHandle = file;
        move.StartingVcn.QuadPart = static_cast<LONGLONG>(vcn);
        move.StartingLcn.QuadPart = static_cast<LONGLONG>(first);
        move.ClusterCount = static_cast<DWORD>(count);
        DWORD returned = 0;
        win_check(::DeviceIoControl(context.volume.value, FSCTL_MOVE_FILE, &move, sizeof(move), nullptr, 0, &returned, nullptr), "move FAT32 file extent");
        context.guard();
      }
      vcn += count; first += count;
    }
  }
  context.verify(file, plan);
}

void write_prepared_fat32_file(HANDLE source, HANDLE target, const MountIdentity& expected,
                               std::uint64_t boundary, const Fat32AllocationPlan& plan,
                               const std::function<bool()>& cancelled,
                               const std::function<void(std::uint64_t)>& progress) {
  PlacementContext context(target, expected, boundary, plan, cancelled);
  context.verify(target, plan);
  BY_HANDLE_FILE_INFORMATION before{}, destination{};
  win_check(::GetFileInformationByHandle(source, &before), "inspect FAT32 copy source");
  win_check(::GetFileInformationByHandle(target, &destination), "inspect FAT32 copy target");
  detail::require_geometry(::GetFileType(source) == FILE_TYPE_DISK &&
      !(before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
      ((static_cast<std::uint64_t>(before.nFileSizeHigh) << 32) | before.nFileSizeLow) == plan.file_bytes);
  detail::require_geometry(before.dwVolumeSerialNumber != destination.dwVolumeSerialNumber ||
      before.nFileIndexHigh != destination.nFileIndexHigh || before.nFileIndexLow != destination.nFileIndexLow);
  security::SecureBuffer buffer(64 * 1024);
  LARGE_INTEGER zero{};
  win_check(::SetFilePointerEx(source, zero, nullptr, FILE_BEGIN), "rewind FAT32 copy source");
  win_check(::SetFilePointerEx(target, zero, nullptr, FILE_BEGIN), "rewind FAT32 copy target");
  std::uint64_t copied = 0;
  while (copied < plan.file_bytes) {
    context.guard();
    const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), plan.file_bytes - copied));
    DWORD read = 0;
    win_check(::ReadFile(source, buffer.data(), wanted, &read, nullptr), "read FAT32 copy source");
    if (read != wanted) throw std::system_error(ERROR_HANDLE_EOF, std::system_category(), "FAT32 copy source truncated");
    DWORD written = 0;
    while (written < read) {
      context.guard();
      DWORD part = 0;
      win_check(::WriteFile(target, buffer.data() + written, read - written, &part, nullptr), "write prepared FAT32 file");
      if (part == 0 || part > read - written) throw std::system_error(ERROR_WRITE_FAULT, std::system_category(), "invalid FAT32 write length");
      written += part;
    }
    buffer.clear();
    copied += read;
    if (progress) progress(copied);
  }
  context.guard();
  win_check(::FlushFileBuffers(target), "flush prepared FAT32 file");
  BY_HANDLE_FILE_INFORMATION after{};
  win_check(::GetFileInformationByHandle(source, &after), "recheck FAT32 copy source");
  detail::require_geometry(before.nFileSizeHigh == after.nFileSizeHigh && before.nFileSizeLow == after.nFileSizeLow &&
      before.ftLastWriteTime.dwHighDateTime == after.ftLastWriteTime.dwHighDateTime &&
      before.ftLastWriteTime.dwLowDateTime == after.ftLastWriteTime.dwLowDateTime);
  context.verify(target, plan);
}
} // namespace usb_vault::volume
