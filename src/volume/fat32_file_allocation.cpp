#include "usb_vault/volume/fat32_file_allocation.h"
#include "volume/fat32_file_allocation_detail.h"
#include <winioctl.h>
#include <cstring>

namespace usb_vault::volume {
namespace {
[[noreturn]] void fail(DWORD error) {
  throw std::system_error(static_cast<int>(error),std::system_category(),"inspect FAT32 file allocation");
}
void check_file(HANDLE file, std::uint64_t bytes, std::uint64_t allocated) {
  FILE_STANDARD_INFO info{};
  if (!::GetFileInformationByHandleEx(file,FileStandardInfo,&info,sizeof(info))) fail(::GetLastError());
  detail::require_geometry(!info.Directory && !info.DeletePending &&
      info.EndOfFile.QuadPart >= 0 && static_cast<std::uint64_t>(info.EndOfFile.QuadPart)==bytes &&
      info.AllocationSize.QuadPart >= 0 && static_cast<std::uint64_t>(info.AllocationSize.QuadPart)==allocated);
}
}
std::vector<Fat32ClusterRun> read_fat32_file_allocation(
    HANDLE file, std::uint64_t expected_file_bytes,
    std::uint32_t cluster_size, std::uint64_t volume_clusters) {
  detail::require_geometry(file!=nullptr && file!=INVALID_HANDLE_VALUE &&
      cluster_size>=512 && cluster_size<=524288 && (cluster_size&(cluster_size-1))==0);
  if(expected_file_bytes>UINT32_MAX) throw std::length_error("file exceeds FAT32 size limit");
  const auto clusters=expected_file_bytes/cluster_size+(expected_file_bytes%cluster_size!=0);
  detail::Fat32ExtentCollector collector(clusters,volume_clusters);
  check_file(file,expected_file_bytes,clusters*cluster_size);
  std::vector<std::byte> buffer(1U<<20);
  while(!collector.complete()) {
    STARTING_VCN_INPUT_BUFFER input{};input.StartingVcn.QuadPart=static_cast<LONGLONG>(collector.next());
    DWORD returned=0;
    const BOOL ok=::DeviceIoControl(file,FSCTL_GET_RETRIEVAL_POINTERS,&input,sizeof(input),
        buffer.data(),static_cast<DWORD>(buffer.size()),&returned,nullptr);
    const auto error=ok?ERROR_SUCCESS : ::GetLastError();
    if(!ok && error!=ERROR_MORE_DATA) fail(error);
    constexpr auto header=offsetof(RETRIEVAL_POINTERS_BUFFER,Extents);
    constexpr auto stride=sizeof(LARGE_INTEGER)*2;
    detail::require_geometry(returned>=header && returned<=buffer.size());
    const auto* frame=reinterpret_cast<const RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
    detail::require_geometry(frame->ExtentCount>0 && frame->ExtentCount<=(returned-header)/stride);
    std::vector<detail::Fat32Extent> extents;extents.reserve(frame->ExtentCount);
    for(DWORD i=0;i<frame->ExtentCount;++i) {
      // Copy from the variable-sized kernel response without indexing beyond
      // the SDK structure's single declared trailing-array element.
      LARGE_INTEGER pair[2]{};
      std::memcpy(pair,buffer.data()+header+static_cast<std::size_t>(i)*stride,stride);
      extents.push_back({pair[0].QuadPart,pair[1].QuadPart});
    }
    collector.consume(frame->StartingVcn.QuadPart,extents);
    detail::require_geometry(ok?collector.complete():!collector.complete());
  }
  check_file(file,expected_file_bytes,clusters*cluster_size);
  return collector.finish();
}
}  // namespace usb_vault::volume
