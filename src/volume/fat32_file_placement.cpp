#include "usb_vault/volume/fat32_file_placement.h"
#include "usb_vault/volume/fat32_file_allocation.h"
#include "volume/fat32_volume_scan.h"
#include "volume/fat32_allocation_plan_detail.h"
#include <winioctl.h>
#include <algorithm>
#include <limits>
#include <system_error>
namespace usb_vault::volume {
namespace {
struct OwnedHandle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~OwnedHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};
void win_check(BOOL ok, const char* operation) {
  if (!ok) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), operation);
}
}
void prepare_fat32_file(HANDLE file, const MountIdentity& expected,
                        std::uint64_t boundary, const Fat32AllocationPlan& plan,
                        const std::function<bool()>& cancelled) {
  using detail::require_geometry;
  require_geometry(expected.drive_letter >= L'A' && expected.drive_letter <= L'Z' && expected.protected_outer_writable);
  VolumeProvider provider;
  const auto guard = [&] {
    if (cancelled && cancelled()) throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    require_geometry(provider.mount_identity(expected.drive_letter) == expected);
  };
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
  OwnedHandle volume;
  const auto raw = std::wstring(LR"(\\?\GLOBALROOT)") + device;
  volume.value = ::CreateFileW(raw.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  win_check(volume.value != INVALID_HANDLE_VALUE, "open FAT32 placement volume");
  const auto geometry = detail::read_fat32_geometry(volume.value, expected.disk_length, boundary);
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
  static_cast<void>(read_fat32_file_allocation(file, 0, geometry.cluster_size, geometry.cluster_count));
  guard();
  LARGE_INTEGER eof{}; eof.QuadPart = static_cast<LONGLONG>(plan.file_bytes);
  win_check(::SetFilePointerEx(file, eof, nullptr, FILE_BEGIN), "seek FAT32 allocation end");
  win_check(::SetEndOfFile(file), "allocate FAT32 file");
  guard();
  std::uint64_t vcn = 0;
  for (const auto& destination : plan.runs) {
    auto first = destination.first_cluster;
    const auto end = first + destination.cluster_count;
    while (first < end) {
      guard();
      const auto current = read_fat32_file_allocation(file, plan.file_bytes, geometry.cluster_size, geometry.cluster_count);
      std::uint64_t base = 0, source = 0, remaining = 0;
      for (const auto& run : current) {
        if (vcn < base + run.cluster_count) { source = run.first_cluster + vcn - base; remaining = base + run.cluster_count - vcn; break; }
        base += run.cluster_count;
      }
      require_geometry(remaining > 0);
      auto count = std::min(end - first, remaining);
      if (source != first) {
        count = detail::free_fat32_prefix(volume.value, geometry, first, count);
        if (count == 0) throw std::system_error(std::make_error_code(std::errc::device_or_resource_busy), "FAT32 destination occupied");
        guard();
        MOVE_FILE_DATA move{};
        move.FileHandle = file;
        move.StartingVcn.QuadPart = static_cast<LONGLONG>(vcn);
        move.StartingLcn.QuadPart = static_cast<LONGLONG>(first);
        move.ClusterCount = static_cast<DWORD>(count);
        DWORD returned = 0;
        win_check(::DeviceIoControl(volume.value, FSCTL_MOVE_FILE, &move, sizeof(move), nullptr, 0, &returned, nullptr), "move FAT32 file extent");
        guard();
      }
      vcn += count; first += count;
    }
  }
  require_geometry(read_fat32_file_allocation(file, plan.file_bytes, geometry.cluster_size, geometry.cluster_count) == plan.runs);
  guard();
}
}
