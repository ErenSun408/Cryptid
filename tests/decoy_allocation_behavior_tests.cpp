#include "usb_vault/volume/fat32_file_transfer.h"
#include "usb_vault/volume/fat32_file_placement.h"
#include "usb_vault/volume/fat32_file_allocation.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume/fat32_safe_space.h"
#include "volume_creation/veracrypt_format_abi.h"
#include <Windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace fs = std::filesystem;
namespace abi = usb_vault::volume_creation::detail::format_abi;
using namespace usb_vault;
constexpr std::uint64_t MiB = 1024 * 1024;
void need(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Handle {
  HANDLE h{INVALID_HANDLE_VALUE};
  Handle() = default;
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : h(std::exchange(other.h,INVALID_HANDLE_VALUE)) {}
  ~Handle() { close(); }
  void close() { if (h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); h = INVALID_HANDLE_VALUE; } }
};
struct Backend {
  HMODULE module{};
  abi::Shutdown shutdown{};
  ~Backend() { if (shutdown) shutdown(); if (module) ::FreeLibrary(module); }
  template<class T> T symbol(const char* name) {
    auto p = ::GetProcAddress(module, name); need(p != nullptr, "missing export");
    return reinterpret_cast<T>(p);
  }
};
security::SecureBuffer secret(const std::string& value) {
  security::SecureBuffer result(value.size());
  std::memcpy(result.data(), value.data(), value.size()); return result;
}
std::array<unsigned char,32> digest(const fs::path& path, std::uint64_t start, std::uint64_t size) {
  struct Hash {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
  } state;
  need(BCryptOpenAlgorithmProvider(&state.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"hash algorithm");
  need(BCryptCreateHash(state.algorithm,&state.hash,nullptr,0,nullptr,0,0)>=0,"hash create");
  std::ifstream input(path,std::ios::binary); input.seekg(static_cast<std::streamoff>(start));
  std::vector<unsigned char> buffer(MiB);
  while(size) {
    const auto n=static_cast<ULONG>(std::min<std::uint64_t>(size,buffer.size()));
    input.read(reinterpret_cast<char*>(buffer.data()),n); need(input.good(),"hash read");
    need(BCryptHashData(state.hash,buffer.data(),n,0)>=0,"hash data"); size-=n;
  }
  std::array<unsigned char,32> result{};
  need(BCryptFinishHash(state.hash,result.data(),static_cast<ULONG>(result.size()),0)>=0,"hash finish");
  return result;
}
struct Mounted {
  volume::VolumeProvider provider;
  fs::path path;
  security::SecureBuffer outer{secret("DisposableOuterP3259")};
  security::SecureBuffer hidden{secret("DisposableHiddenP3259")};
  wchar_t drive{}; bool active{};
  std::wstring root() const { return std::wstring(1,drive)+L":\\"; }
  void open() {
    need(!active,"already mounted");
    volume::MountOptions opts{.drive_letter=drive,.notify_shell=false};
    opts.hidden_volume_protection=volume::HiddenVolumeProtection{.password=&hidden};
    static_cast<void>(provider.mount_file(path.native(),outer,opts)); active=true;
    need(type()==volume::VolumeType::outer,"not protected outer");
  }
  volume::VolumeType type() {
    for(const auto& v:provider.mounted_volumes()) if(v.drive_letter==drive) return v.type;
    return volume::VolumeType::unknown;
  }
  bool close() {
    if(!active) return false;
    auto result=provider.unmount(drive,{.force=true,.notify_shell=false}); active=false;
    return result.hidden_volume_protection_triggered;
  }
  ~Mounted() { if(active) { try { close(); } catch(...) { std::cerr<<"CLEANUP FAILED: test mount remains\n"; } } }
};
std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0]|(p[1]<<8)); }
std::uint32_t u32(const unsigned char* p) { return p[0]|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24); }
struct Layout { std::uint64_t data{},cluster{},clusters{},boundary{}; int fat{}; };
Handle raw(wchar_t drive) {
  Handle result;
  const auto device=std::wstring(L"\\\\.\\")+drive+L":";
  result.h=::CreateFileW(device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  need(result.h!=INVALID_HANDLE_VALUE,"raw open requires elevation");
  return result;
}
Layout layout(wchar_t drive,std::uint64_t boundary) {
  auto handle=raw(drive);
  std::array<unsigned char,512> boot{}; DWORD bytes{};
  need(::ReadFile(handle.h,boot.data(),512,&bytes,nullptr)&&bytes==512,"boot read");
  need(u16(boot.data()+11)==512 && boot[13]!=0 && boot[510]==0x55 && boot[511]==0xaa,"invalid FAT BPB");
  const auto reserved=u16(boot.data()+14),roots=u16(boot.data()+17);
  const std::uint64_t fatsecs=u16(boot.data()+22)?u16(boot.data()+22):u32(boot.data()+36);
  const std::uint64_t sectors=u16(boot.data()+19)?u16(boot.data()+19):u32(boot.data()+32);
  const auto data=reserved+boot[16]*fatsecs+(roots*32+511)/512;
  need(sectors>data && fatsecs>0 && boot[16]==2,"invalid FAT geometry");
  Layout result{data*512,std::uint64_t(boot[13])*512,(sectors-data)/boot[13],boundary,0};
  result.fat=result.clusters<4085?12:result.clusters<65525?16:32;
  need(boundary>result.data && boundary<sectors*512,"protection boundary outside data");
  return result;
}
struct Space { std::uint64_t all{},before{},tail{}; };
Space snapshot(wchar_t drive,const Layout& l) {
  auto handle=raw(drive); STARTING_LCN_INPUT_BUFFER input{};
  std::vector<unsigned char> buffer(MiB); DWORD bytes{};
  need(::DeviceIoControl(handle.h,FSCTL_GET_VOLUME_BITMAP,&input,sizeof(input),buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)!=FALSE,"bitmap read");
  auto* bitmap=reinterpret_cast<VOLUME_BITMAP_BUFFER*>(buffer.data());
  constexpr auto header=offsetof(VOLUME_BITMAP_BUFFER,Buffer);
  need(bytes>=header && bitmap->StartingLcn.QuadPart==0 && bitmap->BitmapSize.QuadPart>=static_cast<LONGLONG>(l.clusters)
       && (bytes-header)*8>=l.clusters,"bitmap coverage");
  Space result;
  for(std::uint64_t i=0;i<l.clusters;++i) {
    const bool free=(bitmap->Buffer[i/8]&(1U<<(i%8)))==0;
    if(free) result.all+=l.cluster;
    if(l.data+(i+1)*l.cluster<=l.boundary) {
      if(free) result.before+=l.cluster;
      result.tail=free?result.tail+l.cluster:0;
    }
  }
  DWORD spc{},bps{},free{},total{};
  const auto root=std::wstring(1,drive)+L":\\";
  need(::GetDiskFreeSpaceW(root.c_str(),&spc,&bps,&free,&total)!=FALSE,"space check");
  // Independent cross-check of the BPB/bitmap interpretation on every snapshot.
  need(std::uint64_t(spc)*bps==l.cluster && total==l.clusters && std::uint64_t(free)*l.cluster==result.all,"BPB/bitmap/free-count mismatch");
  return result;
}
using Extents = std::vector<std::pair<std::uint64_t,std::uint64_t>>;
Extents extent_ranges(HANDLE file) {
  STARTING_VCN_INPUT_BUFFER input{}; DWORD bytes{}; std::vector<unsigned char> buffer(65536);
  need(::DeviceIoControl(file,FSCTL_GET_RETRIEVAL_POINTERS,&input,sizeof(input),buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr)!=FALSE,"file extents");
  auto* ext=reinterpret_cast<RETRIEVAL_POINTERS_BUFFER*>(buffer.data());
  need(bytes>=offsetof(RETRIEVAL_POINTERS_BUFFER,Extents) && ext->ExtentCount>0 &&
      ext->ExtentCount<=(bytes - offsetof(RETRIEVAL_POINTERS_BUFFER,Extents))/sizeof(ext->Extents[0]),"extent shape");
  Extents result;
  LONGLONG prev=ext->StartingVcn.QuadPart;
  for(DWORD i=0;i<ext->ExtentCount;++i) {
    const auto& e=ext->Extents[i]; need(e.Lcn.QuadPart>=0&&e.NextVcn.QuadPart>prev,"sparse or invalid extent");
    result.emplace_back(static_cast<std::uint64_t>(e.Lcn.QuadPart),static_cast<std::uint64_t>(e.Lcn.QuadPart+e.NextVcn.QuadPart-prev)); prev=e.NextVcn.QuadPart;
  }
  return result;
}
void seed_file(const std::wstring& path) {
  Handle file; file.h=::CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
  need(file.h!=INVALID_HANDLE_VALUE,"seed create"); std::vector<unsigned char> block(MiB,0x32); DWORD bytes{};
  need(::WriteFile(file.h,block.data(),static_cast<DWORD>(block.size()),&bytes,nullptr)&&bytes==block.size()&&::FlushFileBuffers(file.h),"seed write");
}
int recovery_run(int argc,wchar_t** argv);
int placement_run(int argc,wchar_t** argv);
int sequential_run(int argc,wchar_t** argv);
int fault_run(wchar_t** argv);
int measurement_helper_run(wchar_t** argv);
int import_helper_run(wchar_t** argv);
int abrupt_test_writer(wchar_t* path);
int wmain(int argc,wchar_t** argv) {
  if(argc==5 && std::wstring_view(argv[1])==L"--import-helper-run") return import_helper_run(argv);
  if(argc==5 && std::wstring_view(argv[1])==L"--measurement-helper-run") return measurement_helper_run(argv);
  if(argc==3 && std::wstring_view(argv[1])==L"--abrupt-disposable-writer") return abrupt_test_writer(argv[2]);
  if(argc==5 && std::wstring_view(argv[1])==L"--fault-disposable-run") return fault_run(argv);
  if(argc==5 && (std::wstring_view(argv[1])==L"--sequential-disposable-run" || std::wstring_view(argv[1])==L"--production-placement-run" || std::wstring_view(argv[1])==L"--production-write-run" || std::wstring_view(argv[1])==L"--production-transfer-run")) return sequential_run(argc,argv);
  if(argc==5 && (std::wstring_view(argv[1])==L"--placement-disposable-run" || std::wstring_view(argv[1])==L"--multirun-disposable-run")) return placement_run(argc,argv);
  if(argc==5 && std::wstring_view(argv[1])==L"--recover-disposable-run") return recovery_run(argc,argv);
  bool output_created=false;
  try {
    const bool prepare=argc==4&&std::wstring_view(argv[1])==L"--prepare-disposable-base";
    const bool basic=argc==4&&std::wstring_view(argv[1])==L"--new-disposable-run-basic";
    need(argc==4&&(prepare||basic||std::wstring_view(argv[1])==L"--new-disposable-run"),"requires explicit new-run flag, candidate DLL, NEW output directory");
    const fs::path dll=argv[2],output=argv[3];
    need(dll.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"absolute paths and new directory required");
    need(fs::create_directory(output),"create output directory");
    output_created=true;
    std::ofstream log(output/L"results.csv"); log<<"host_mib,repeat,scenario,fat,cluster,measurement,before_free,before_tail,success_bytes,after_free,after_tail,deleted_bytes,deleted_unreused_bytes,write_error,flush_error,triggered,hidden_unchanged\n"; log.flush();
    Backend backend; backend.module=::LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    need(backend.module!=nullptr,"load candidate DLL");
    need(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")()==0,"initialize DLL");
    backend.shutdown=backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    auto format=backend.symbol<abi::FormatBinary>("VeraCryptFormatBinary");
    auto query=backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry");
    const std::string op="DisposableOuterP3259",hp="DisposableHiddenP3259";
    for(const unsigned size:{32U,128U}) {
      const auto base=output/(L"base-"+std::to_wstring(size)+L".hc");
      abi::Options options{.path=base.c_str(),.is_device=FALSE,.password=op.c_str(),.keyfiles=nullptr,.pim=0,.size=size*MiB,
        .encryption_algorithm=L"AES",.hash_algorithm=L"SHA-512",.filesystem=L"FAT",.cluster_size=0,.quick_format=TRUE,
        .dynamic_format=FALSE,.fast_create_file=FALSE,.progress_callback=nullptr,.progress_user_data=nullptr,.hidden_volume=FALSE,.hidden_volume_host_size=0};
      need(format(&options,static_cast<std::uint32_t>(op.size()))==0,"format outer fixture");
      options.password=hp.c_str();options.size=size*MiB/2;options.hidden_volume=TRUE;options.hidden_volume_host_size=size*MiB;
      need(format(&options,static_cast<std::uint32_t>(hp.size()))==0,"format hidden fixture");
      std::ifstream headers(base,std::ios::binary); std::array<unsigned char,512> a{},b{};
      headers.read(reinterpret_cast<char*>(a.data()),512); headers.seekg(65536);headers.read(reinterpret_cast<char*>(b.data()),512);need(headers.good(),"read headers");headers.close();
      std::array<std::uint64_t,4> g{};
      need(query(a.data(),512,b.data(),512,reinterpret_cast<const unsigned char*>(op.data()),static_cast<std::uint32_t>(op.size()),reinterpret_cast<const unsigned char*>(hp.data()),static_cast<std::uint32_t>(hp.size()),size*MiB,g.data(),32)==0,"fixture geometry");
      if(prepare) continue;
      const auto hidden_hash=digest(base,g[2],g[3]);
      for(unsigned rep=1;rep<=2;++rep) for(unsigned scenario=0;scenario<6;++scenario) {
        Mounted m;m.path=output/(L"case-"+std::to_wstring(size)+L"-"+std::to_wstring(rep)+L"-"+std::to_wstring(scenario)+L".hc");
        need(fs::copy_file(base,m.path,fs::copy_options::none),"copy new fixture");
        DWORD used=::GetLogicalDrives();need(used!=0,"drive list");
        for(auto& v:m.provider.mounted_volumes())used|=1UL<<(v.drive_letter-L'A');
        for(wchar_t d=L'Y';d>=L'D';--d) { wchar_t name[]{d,L':',0};wchar_t target[1024]{};if(!(used&(1UL<<(d-L'A')))&&!::QueryDosDeviceW(name,target,1024)){m.drive=d;break;} }
        need(m.drive!=0,"no free drive");m.open();
        Layout l;
        if(basic) {
          DWORD spc{},bps{},free{},total{};
          need(::GetDiskFreeSpaceW(m.root().c_str(),&spc,&bps,&free,&total)!=FALSE,"basic geometry");
          l.cluster=std::uint64_t(spc)*bps;l.clusters=total;l.fat=total<65525?16:32;
        } else l=layout(m.drive,g[2]-g[0]);
        auto measure=[&]() {
          if(!basic)return snapshot(m.drive,l);
          DWORD spc{},bps{},free{},total{};
          need(::GetDiskFreeSpaceW(m.root().c_str(),&spc,&bps,&free,&total)!=FALSE,"basic space");

const auto free_bytes=std::uint64_t(free)*l.cluster;
          // Subtract an upper bound for all clusters at/after protection,
          // including one possible crossing cluster: this is a lower bound
          // on free space BEFORE protection, not a promised write budget.
          const auto remaining=g[0]+g[1]-g[2];
          const auto excluded=((remaining+l.cluster-1)/l.cluster+1)*l.cluster;
          return Space{free_bytes,free_bytes>excluded?free_bytes-excluded:0,UINT64_MAX};
        };
        Extents deleted;
        const unsigned count=size*3/8;
        if(scenario) {
          for(unsigned i=0;i<count;++i) seed_file(m.root()+L"seed-"+std::to_wstring(i)+L".bin");
          for(unsigned i=0;i<count;++i) {
            const bool remove=(scenario==3||scenario==4)?(i%2==0):(i<count/2);
            if(remove) {
              const auto name=m.root()+L"seed-"+std::to_wstring(i)+L".bin";
              Handle seed;seed.h=::CreateFileW(name.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
              need(seed.h!=INVALID_HANDLE_VALUE,"seed extent open");
              auto ranges=extent_ranges(seed.h);deleted.insert(deleted.end(),ranges.begin(),ranges.end());seed.close();
              need(::DeleteFileW(name.c_str()),"delete only owned seed");
            }
          }
          if(scenario==2||scenario==4) {m.close();m.open();}
        }
        const auto before=measure();
        const bool extend=scenario==5;
        const auto target=extend?m.root()+L"seed-"+std::to_wstring(count-1)+L".bin":m.root()+L"fill.bin";
        Handle file;file.h=::CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,extend?OPEN_EXISTING:CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
        need(file.h!=INVALID_HANDLE_VALUE,"fill open");
        if(extend) {LARGE_INTEGER zero{};need(::SetFilePointerEx(file.h,zero,nullptr,FILE_END),"seek append");}
        std::vector<unsigned char> block(65536,0x61);std::uint64_t successful=0;DWORD write_error=0,flush_error=0;
        for(unsigned i=0;i<size*16+1;++i) {
          DWORD bytes{};const BOOL ok=::WriteFile(file.h,block.data(),static_cast<DWORD>(block.size()),&bytes,nullptr);
          write_error=ok ? 0 : ::GetLastError();const BOOL flushed=ok?::FlushFileBuffers(file.h):FALSE;
          flush_error=ok&&!flushed?::GetLastError():0;
          if(!ok||!flushed||bytes!=block.size())break;
          successful+=bytes;
        }
        const auto ranges=extent_ranges(file.h);file.close();
        std::uint64_t deleted_clusters=0,reused_clusters=0;
        for(const auto& old:deleted) {
          deleted_clusters+=old.second-old.first;
          for(const auto& now:ranges) {
            const auto begin=std::max(old.first,now.first),end=std::min(old.second,now.second);
            if(end>begin)reused_clusters+=end-begin;
          }
        }
        need(reused_clusters<=deleted_clusters,"extent intersection invalid");
        const bool triggered=m.type()==volume::VolumeType::outer_write_prevented;
        const auto after=measure();
        const bool flag=m.close();need(flag==triggered,"trigger flag mismatch");
        const bool unchanged=digest(m.path,g[2],g[3])==hidden_hash;
        log<<size<<','<<rep<<','<<scenario<<','<<l.fat<<','<<l.cluster<<','<<(basic?"lower_bound":"bitmap")<<','<<before.before<<','<<before.tail<<','<<successful<<','<<after.before<<','<<after.tail<<','<<deleted_clusters*l.cluster<<','<<(deleted_clusters-reused_clusters)*l.cluster<<','<<write_error<<','<<flush_error<<','<<triggered<<','<<unchanged<<'\n';log.flush();
        std::cout<<"completed "<<size<<'/'<<rep<<'/'<<scenario<<std::endl;
        need(triggered&&unchanged,"protection absent or hidden region changed");
      }
    }
    std::ofstream(output/L"complete.txt")<<(prepare?"Prepared two disposable bases; no allocation scenarios executed\n":"PASS 24 scenarios; only disposable fixtures written\n");
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';if(output_created)std::ofstream(fs::path(argv[3])/L"failure.txt")<<e.what()<<'\n';return 1;}
}

struct UploadResult {
  std::uint64_t written{};
  DWORD error{};
  bool complete{};
};
UploadResult upload_file(const std::wstring& path,std::uint64_t size,bool overwrite=false) {
  UploadResult result;
  Handle file;file.h=::CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,overwrite?OPEN_EXISTING:CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
  if(file.h==INVALID_HANDLE_VALUE) {result.error=::GetLastError();return result;}
  std::vector<unsigned char> block(65536,0x75);
  while(result.written<size) {
    const auto n=static_cast<DWORD>(std::min<std::uint64_t>(block.size(),size-result.written));
    DWORD count{};
    if(!::WriteFile(file.h,block.data(),n,&count,nullptr)) {result.error=::GetLastError();return result;}
    if(count!=n) {result.error=ERROR_WRITE_FAULT;return result;}
    if(!::FlushFileBuffers(file.h)) {result.error=::GetLastError();return result;}
    result.written+=count;
  }
  result.complete=true;return result;
}
bool verify_upload(const std::wstring& path,std::uint64_t size) {
  Handle file;file.h=::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  if(file.h==INVALID_HANDLE_VALUE)return false;
  LARGE_INTEGER length{};
  if(!::GetFileSizeEx(file.h,&length)||length.QuadPart!=static_cast<LONGLONG>(size))return false;
  std::vector<unsigned char> buffer(65536);
  while(size) {
    const auto n=static_cast<DWORD>(std::min<std::uint64_t>(size,buffer.size()));DWORD count{};
    if(!::ReadFile(file.h,buffer.data(),n,&count,nullptr)||count!=n)return false;
    if(!std::all_of(buffer.begin(),buffer.begin()+n,[](auto b){return b==0x75;}))return false;
    size-=n;
  }
  return true;
}
void choose_drive(Mounted& m) {
  DWORD used=::GetLogicalDrives();need(used!=0,"drive list");
  for(const auto& v:m.provider.mounted_volumes()) {
    need(v.drive_letter>=L'A'&&v.drive_letter<=L'Z',"invalid driver letter");
    used|=1UL<<(v.drive_letter-L'A');
  }
  for(wchar_t d=L'Y';d>=L'D';--d) {
    wchar_t name[]{d,L':',0},target[1024]{};
    if(!(used&(1UL<<(d-L'A')))&&!::QueryDosDeviceW(name,target,1024)){m.drive=d;return;}
  }
  throw std::runtime_error("no free test drive");
}
std::uint64_t free_before_lower_bound(Mounted& m,const std::array<std::uint64_t,4>& g) {
  DWORD spc{},bps{},free{},total{};
  need(::GetDiskFreeSpaceW(m.root().c_str(),&spc,&bps,&free,&total)!=FALSE,"free space");
  const auto cluster=std::uint64_t(spc)*bps;need(cluster>0,"cluster size");
  const auto excluded=((g[0]+g[1]-g[2]+cluster-1)/cluster+1)*cluster;
  const auto available=std::uint64_t(free)*cluster;
  return available>excluded?available-excluded:0;
}
int recovery_run(int argc,wchar_t** argv) {
  (void)argc;
  bool created=false;
  try {
    const fs::path dll=argv[2],source=argv[3],output=argv[4];
    need(dll.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"recovery requires absolute paths and NEW output directory");
    const auto base=source/L"base-128.hc";
    need(fs::is_regular_file(source/L"complete.txt")&&fs::file_size(base)==128*MiB,"expected completed disposable allocation run");
    need(fs::create_directory(output),"new recovery directory");created=true;
    std::ofstream log(output/L"results.csv");
    log<<"repeat,stage,requested_bytes,before_free_lower,written_bytes,error,complete,persisted_after_remount,triggered,hidden_unchanged\n";log.flush();
    Backend backend;backend.module=::LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    need(backend.module!=nullptr,"load candidate DLL");
    need(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")()==0,"initialize DLL");
    backend.shutdown=backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    std::ifstream headers(base,std::ios::binary);std::array<unsigned char,512>a{},b{};
    headers.read(reinterpret_cast<char*>(a.data()),512);headers.seekg(65536);headers.read(reinterpret_cast<char*>(b.data()),512);need(headers.good(),"read base headers");headers.close();
    const std::string op="DisposableOuterP3259",hp="DisposableHiddenP3259";std::array<std::uint64_t,4>g{};
    need(backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry")(a.data(),512,b.data(),512,reinterpret_cast<const unsigned char*>(op.data()),static_cast<std::uint32_t>(op.size()),reinterpret_cast<const unsigned char*>(hp.data()),static_cast<std::uint32_t>(hp.size()),128*MiB,g.data(),32)==0,"query geometry");
    const auto expected=digest(base,g[2],g[3]);
    const std::array<std::uint64_t,6> sizes{4096,65536,MiB,4*MiB,16*MiB,23*MiB};
    for(unsigned rep=1;rep<=3;++rep) {
      Mounted initial;initial.path=output/(L"triggered-"+std::to_wstring(rep)+L".hc");
      need(fs::copy_file(base,initial.path,fs::copy_options::none),"copy initial");
      choose_drive(initial);initial.open();
      for(unsigned i=0;i<48;++i)seed_file(initial.root()+L"seed-"+std::to_wstring(i)+L".bin");
      for(unsigned i=0;i<48;i+=2)need(::DeleteFileW((initial.root()+L"seed-"+std::to_wstring(i)+L".bin").c_str()),"delete own seed");
      const auto fill=upload_file(initial.root()+L"fill.bin",128*MiB);
      need(!fill.complete&&initial.type()==volume::VolumeType::outer_write_prevented,"must reproduce protection trigger");
      const auto free=free_before_lower_bound(initial,g);
      need(free>=23*MiB,"not enough retained free space for requested experiment");
      for(const auto size:sizes) {
        auto result=upload_file(initial.root()+L"locked-"+std::to_wstring(size)+L".bin",size);
        const bool triggered=initial.type()==volume::VolumeType::outer_write_prevented;
        log<<rep<<",same_mount,"<<size<<','<<free<<','<<result.written<<','<<result.error<<','<<result.complete<<",0,"<<triggered<<",NA\n";log.flush();
        need(!result.complete&&triggered,"same-mount write unexpectedly accepted");
      }
      need(initial.close(),"trigger flag missing");
      need(digest(initial.path,g[2],g[3])==expected,"initial hidden region changed");
      log<<rep<<",baseline,0,"<<free<<','<<fill.written<<','<<fill.error<<",0,0,1,1\n";log.flush();

      for(unsigned control=0;control<4;++control) {
        const bool overwrite=control==0;
        const std::uint64_t size=control==0?MiB:control==1?4096:control==2?MiB:23*MiB;
        Mounted m;m.path=output/(L"control-"+std::to_wstring(rep)+L"-"+std::to_wstring(control)+L".hc");
        need(fs::copy_file(initial.path,m.path,fs::copy_options::none),"copy control");
        choose_drive(m);m.open();
        DWORD cleanup_error=0;
        if(!overwrite&&!::DeleteFileW((m.root()+L"fill.bin").c_str()))cleanup_error=::GetLastError();
        const auto before=free_before_lower_bound(m,g);
        const auto filename=overwrite?L"seed-1.bin":L"retry.bin";
        UploadResult result;
        if(cleanup_error)result.error=cleanup_error;
        else result=upload_file(m.root()+filename,size,overwrite);
        const bool triggered=m.type()==volume::VolumeType::outer_write_prevented;
        const bool immediate=result.complete&&verify_upload(m.root()+filename,size);m.close();
        bool persisted=false;
        if(result.complete){m.open();persisted=verify_upload(m.root()+filename,size);m.close();}
        const bool unchanged=digest(m.path,g[2],g[3])==expected;
        log<<rep<<','<<(overwrite?"overwrite_existing":cleanup_error?"delete_failed":"delete_fill_then_upload")<<','<<size<<','<<before<<','<<result.written<<','<<result.error<<','<<result.complete<<','<<persisted<<','<<triggered<<','<<unchanged<<std::endl;
        need(unchanged&&(!result.complete||(immediate&&persisted)),"control integrity or persistence failed");
        std::cout<<"control "<<rep<<'/'<<control<<" complete="<<result.complete<<std::endl;
      }
      for(const auto size:sizes) {
        Mounted m;m.path=output/(L"retry-"+std::to_wstring(rep)+L"-"+std::to_wstring(size)+L".hc");
        need(fs::copy_file(initial.path,m.path,fs::copy_options::none),"copy independent retry");
        choose_drive(m);m.open();const auto before=free_before_lower_bound(m,g);
        auto result=upload_file(m.root()+L"retry.bin",size);
        const bool triggered=m.type()==volume::VolumeType::outer_write_prevented;
        const bool immediate=result.complete&&verify_upload(m.root()+L"retry.bin",size);
        m.close();
        bool persisted=false;
        if(result.complete) {m.open();persisted=verify_upload(m.root()+L"retry.bin",size);m.close();}
        const bool unchanged=digest(m.path,g[2],g[3])==expected;
        log<<rep<<",remount,"<<size<<','<<before<<','<<result.written<<','<<result.error<<','<<result.complete<<','<<persisted<<','<<triggered<<','<<unchanged<<'\n';log.flush();
        need(unchanged,"retry changed hidden region");
        need(!result.complete||(immediate&&persisted),"reported success did not persist");
        std::cout<<"retry "<<rep<<" bytes="<<size<<" complete="<<result.complete<<" persisted="<<persisted<<std::endl;
      }
    }
    std::ofstream(output/L"complete.txt")<<"PASS 18 same-mount rejections, 18 independent remount trials and 12 controls; successful files verified after another remount\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';
    if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<'\n';
    return 1;
  }
}


Extents safe_free_runs(Mounted& m,const Layout& l) {
  auto vol=raw(m.drive);STARTING_LCN_INPUT_BUFFER in{};DWORD bytes=0;
  std::vector<unsigned char> bits(MiB);
  need(::DeviceIoControl(vol.h,FSCTL_GET_VOLUME_BITMAP,&in,sizeof(in),bits.data(),static_cast<DWORD>(bits.size()),&bytes,nullptr)!=FALSE,"fresh bitmap");
  auto* bm=reinterpret_cast<VOLUME_BITMAP_BUFFER*>(bits.data());
  need(bytes>=offsetof(VOLUME_BITMAP_BUFFER,Buffer)&&bm->StartingLcn.QuadPart==0&&bm->BitmapSize.QuadPart>=static_cast<LONGLONG>(l.clusters)&&(bytes-offsetof(VOLUME_BITMAP_BUFFER,Buffer))*8>=l.clusters,"fresh bitmap shape");
  Extents result;
  for(std::uint64_t i=0;i<l.clusters&&l.data+(i+1)*l.cluster<=l.boundary;++i) {
    if((bm->Buffer[i/8]&(1U<<(i%8)))==0) {
      if(!result.empty()&&result.back().second==i)result.back().second=i+1;
      else result.emplace_back(i,i+1);
    }
  }
  std::sort(result.begin(),result.end(),[](auto x,auto y){return x.second-x.first>y.second-y.first;});
  return result;
}
struct ExistingFile { fs::path name; std::uint64_t size; std::array<unsigned char,32> hash; };
std::vector<ExistingFile> existing_files(const std::wstring& root) {
  std::vector<ExistingFile> files;
  for(const auto& entry:fs::directory_iterator(root)) {
    if(entry.is_regular_file()&&entry.path().filename()!=L"placed.bin") {
      const auto size=entry.file_size();files.push_back({entry.path().filename(),size,digest(entry.path(),0,size)});
    }
  }
  std::sort(files.begin(),files.end(),[](const auto& x,const auto& y){return x.name<y.name;});
  return files;
}
bool unchanged_files(const std::vector<ExistingFile>& before,const std::vector<ExistingFile>& after) {
  if(before.size()!=after.size())return false;
  for(size_t i=0;i<before.size();++i)if(before[i].name!=after[i].name||before[i].size!=after[i].size||before[i].hash!=after[i].hash)return false;
  return true;
}
unsigned char placement_byte(std::uint64_t offset) {
  return static_cast<unsigned char>((offset*29)^(offset>>8)^(offset>>16)^(offset>>20));
}
bool verify_pattern(const std::wstring& path,std::uint64_t size) {
  if(fs::file_size(path)!=size)return false;
  std::ifstream input(fs::path(path),std::ios::binary);std::vector<unsigned char> block(65536);
  for(std::uint64_t offset=0;offset<size;offset+=block.size()) {
    const auto count=static_cast<size_t>(std::min<std::uint64_t>(block.size(),size-offset));
    input.read(reinterpret_cast<char*>(block.data()),static_cast<std::streamsize>(count));if(!input.good())return false;
    for(size_t i=0;i<count;++i)if(block[i]!=placement_byte(offset+i))return false;
  }
  return true;
}
int placement_run(int argc,wchar_t** argv) {
  (void)argc; bool created=false;
  const bool multi=std::wstring_view(argv[1])==L"--multirun-disposable-run";
  try {
    const fs::path dll=argv[2],source=argv[3],output=argv[4];
    need(dll.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"placement requires absolute paths and NEW output");
    need(fs::is_regular_file(source/L"complete.txt"),"completed recovery source required");
    for(unsigned r=1;r<=2;++r)need(fs::file_size(source/(L"triggered-"+std::to_wstring(r)+L".hc"))==128*MiB,"expected disposable fixture");
    need(fs::create_directory(output),"create placement output");created=true;
    std::ofstream log(output/L"results.csv");
    log<<"repeat,method,size,stage,ok,error,triggered,value"<<std::endl;
    Backend backend;backend.module=::LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    need(backend.module!=nullptr,"load candidate DLL");
    need(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")()==0,"initialize DLL");
    backend.shutdown=backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    const auto sizes=multi?std::vector<std::uint64_t>{8*MiB}:std::vector<std::uint64_t>{4096,MiB};
    for(unsigned rep=1;rep<=2;++rep) for(unsigned method=0;method<2;++method) for(const auto size:sizes) {
      const auto original=source/(L"triggered-"+std::to_wstring(rep)+L".hc");
      std::ifstream headers(original,std::ios::binary);std::array<unsigned char,512>a{},b{};
      headers.read(reinterpret_cast<char*>(a.data()),512);headers.seekg(65536);headers.read(reinterpret_cast<char*>(b.data()),512);need(headers.good(),"headers");headers.close();
      const std::string op="DisposableOuterP3259",hp="DisposableHiddenP3259";std::array<std::uint64_t,4>g{};
      need(backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry")(a.data(),512,b.data(),512,reinterpret_cast<const unsigned char*>(op.data()),static_cast<std::uint32_t>(op.size()),reinterpret_cast<const unsigned char*>(hp.data()),static_cast<std::uint32_t>(hp.size()),128*MiB,g.data(),32)==0,"geometry");
      const auto expected=digest(original,g[2],g[3]);
      Mounted m;m.path=output/(std::to_wstring(rep)+L"-"+std::to_wstring(method)+L"-"+std::to_wstring(size)+L".hc");
      need(fs::copy_file(original,m.path,fs::copy_options::none),"copy fixture");choose_drive(m);m.open();
      auto record=[&](const char* stage,bool ok,DWORD error,std::uint64_t value=0) {
        log<<rep<<','<<method<<','<<size<<','<<stage<<','<<ok<<','<<error<<','<<(m.active&&m.type()==volume::VolumeType::outer_write_prevented)<<','<<value<<std::endl;
        std::cout<<rep<<'/'<<method<<'/'<<size<<' '<<stage<<" ok="<<ok<<" error="<<error<<std::endl;
      };
      Handle vol;const auto device=std::wstring(L"\\\\.\\")+m.drive+L":";
      vol.h=::CreateFileW(device.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
      record("volume_open",vol.h!=INVALID_HANDLE_VALUE,vol.h==INVALID_HANDLE_VALUE?::GetLastError():0);
      bool multi_verified=false;
      const auto originals=multi?existing_files(m.root()):std::vector<ExistingFile>{};
      if(multi)need(originals.size()>=25,"missing existing fixture files");
      Layout l{};std::uint64_t bestStart=0,bestCount=0;
      if(vol.h!=INVALID_HANDLE_VALUE) {
        l=layout(m.drive,g[2]-g[0]);static_cast<void>(snapshot(m.drive,l));
        STARTING_LCN_INPUT_BUFFER in{};std::vector<unsigned char> bits(MiB);DWORD bytes=0;
        need(::DeviceIoControl(vol.h,FSCTL_GET_VOLUME_BITMAP,&in,sizeof(in),bits.data(),static_cast<DWORD>(bits.size()),&bytes,nullptr)!=FALSE,"placement bitmap");
        auto* bm=reinterpret_cast<VOLUME_BITMAP_BUFFER*>(bits.data());
        need(bytes>=offsetof(VOLUME_BITMAP_BUFFER,Buffer)&&bm->StartingLcn.QuadPart==0&&bm->BitmapSize.QuadPart>=static_cast<LONGLONG>(l.clusters)&&(bytes-offsetof(VOLUME_BITMAP_BUFFER,Buffer))*8>=l.clusters,"bitmap shape");
        std::uint64_t start=0,count=0;
        for(std::uint64_t i=0;i<l.clusters&&l.data+(i+1)*l.cluster<=l.boundary;++i) {
          if((bm->Buffer[i/8]&(1U<<(i%8)))==0) {if(!count)start=i;++count;if(count>bestCount){bestStart=start;bestCount=count;}}
          else count=0;
        }
        record("largest_safe_run",true,0,bestCount*l.cluster);
        record("destination_lcn",true,0,bestStart);
      }
      if(multi)need(vol.h!=INVALID_HANDLE_VALUE&&bestCount*l.cluster<size,"multirun requires elevation and fragmented fixture");
      Handle file;file.h=::CreateFileW((m.root()+L"placed.bin").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
      need(file.h!=INVALID_HANDLE_VALUE,"create placed file");
      BOOL ok=FALSE;
      if(method==0){FILE_ALLOCATION_INFO info{};info.AllocationSize.QuadPart=static_cast<LONGLONG>(size);ok=::SetFileInformationByHandle(file.h,FileAllocationInfo,&info,sizeof(info));}
      else {LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(size);need(::SetFilePointerEx(file.h,end,nullptr,FILE_BEGIN),"seek eof");ok=::SetEndOfFile(file.h);}
      record("preallocate",ok!=FALSE,ok? 0 : ::GetLastError());
      if(ok && method==0) {
        LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(size);
        need(::SetFilePointerEx(file.h,end,nullptr,FILE_BEGIN),"set preallocated eof position");
        ok=::SetEndOfFile(file.h);record("set_eof",ok!=FALSE,ok?0 : ::GetLastError());
      }
      if(ok && m.type()==volume::VolumeType::outer) {
        try { const auto ext=extent_ranges(file.h);record("allocated_first_lcn",true,0,ext.front().first); } catch(const std::exception&) { record("initial_extents_unavailable",false,::GetLastError()); }
        if(vol.h!=INVALID_HANDLE_VALUE && (multi||bestCount*l.cluster>=size)) {

          Extents planned;
          if(multi) {
            std::uint64_t vcn=0;const auto clusters=size/l.cluster;
            while(vcn<clusters) {
              const auto runs=safe_free_runs(m,l);need(!runs.empty(),"no safe destination");
              const auto destination=runs.front().first,count=std::min(clusters-vcn,runs.front().second-destination);
              MOVE_FILE_DATA part{};part.FileHandle=file.h;part.StartingVcn.QuadPart=static_cast<LONGLONG>(vcn);part.StartingLcn.QuadPart=static_cast<LONGLONG>(destination);part.ClusterCount=static_cast<DWORD>(count);DWORD bytes=0;
              ok=::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&part,sizeof(part),nullptr,0,&bytes,nullptr);
              record("move_part",ok!=FALSE,ok?0 : ::GetLastError(),count*l.cluster);
              need(ok&&m.type()==volume::VolumeType::outer,"part move failed or protection triggered");
              planned.emplace_back(destination,destination+count);vcn+=count;
            }
            record("part_count",true,0,planned.size());need(planned.size()>1,"not fragmented allocation");
          } else {
            MOVE_FILE_DATA move{};move.FileHandle=file.h;move.StartingLcn.QuadPart=static_cast<LONGLONG>(bestStart);move.ClusterCount=static_cast<DWORD>((size+l.cluster-1)/l.cluster);DWORD bytes=0;
            ok=::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&move,sizeof(move),nullptr,0,&bytes,nullptr);
            planned.emplace_back(bestStart,bestStart+move.ClusterCount);
          }
          record("move",ok!=FALSE,ok?0 : ::GetLastError());
          if(ok) {
            const auto after=extent_ranges(file.h);need(after==planned,"move extent mismatch");
            for(const auto& range:after)need(l.data+range.second*l.cluster<=l.boundary,"moved extent outside safe region");
            record("extents_verified",true,0,after.size());
            LARGE_INTEGER zero{};need(::SetFilePointerEx(file.h,zero,nullptr,FILE_BEGIN),"rewind");
            std::vector<unsigned char> content(static_cast<size_t>(size),0x75);
            if(multi)for(size_t i=0;i<content.size();++i)content[i]=placement_byte(i);
            DWORD written=0;
            ok=::WriteFile(file.h,content.data(),static_cast<DWORD>(size),&written,nullptr);DWORD error=ok? 0 : ::GetLastError();
            if(ok&&written==size){ok=::FlushFileBuffers(file.h);error=ok? 0 : ::GetLastError();}else ok=FALSE;
            record("write_flush",ok!=FALSE,error,written);
            file.close();vol.close();m.close();
            if(ok){m.open();const bool valid=multi?verify_pattern(m.root()+L"placed.bin",size):verify_upload(m.root()+L"placed.bin",size);record("persisted",valid,0);need(valid,"persistence mismatch");
              if(multi){const bool preserved=unchanged_files(originals,existing_files(m.root()));record("existing_files_unchanged",preserved,0,originals.size());need(preserved,"existing files changed");multi_verified=true;}
            }
          }
        } else record("move_not_tested",false,vol.h==INVALID_HANDLE_VALUE?ERROR_ACCESS_DENIED:ERROR_DISK_FULL);
      }
      file.close();vol.close();m.close();const bool unchanged=digest(m.path,g[2],g[3])==expected;
      record("hidden_unchanged",unchanged,0);need(unchanged,"hidden region changed");
      if(multi)need(multi_verified,"multirun did not complete verification");
    }
    std::ofstream(output/L"complete.txt")<<(multi?"PASS 4 multirun trials; new file, existing files and hidden region verified.":"Completed 8 independent protected placement trials; inspect stage rows for skipped moves.")<<std::endl;return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<std::endl;return 1;}
}

int sequential_run(int argc,wchar_t** argv) {
  (void)argc;bool created=false;
  try {
    const fs::path dll=argv[2],source=argv[3],output=argv[4],base=source/L"base-128.hc";
    need(dll.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"sequential requires absolute paths and new output");
    need(fs::is_regular_file(source/L"complete.txt")&&fs::file_size(base)==128*MiB,"completed disposable base required");
    need(fs::create_directory(output),"new sequential output");created=true;
    std::ofstream log(output/L"results.csv");log<<"repeat,file,stage,ok,error,value,lcn,triggered"<<std::endl;
    Backend backend;backend.module=::LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    need(backend.module!=nullptr,"load candidate");need(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")()==0,"initialize");backend.shutdown=backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    std::ifstream headers(base,std::ios::binary);std::array<unsigned char,512>a{},b{};
    headers.read(reinterpret_cast<char*>(a.data()),512);headers.seekg(65536);headers.read(reinterpret_cast<char*>(b.data()),512);need(headers.good(),"headers");headers.close();
    const std::string op="DisposableOuterP3259",hp="DisposableHiddenP3259";std::array<std::uint64_t,4>g{};
    need(backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry")(a.data(),512,b.data(),512,reinterpret_cast<const unsigned char*>(op.data()),static_cast<std::uint32_t>(op.size()),reinterpret_cast<const unsigned char*>(hp.data()),static_cast<std::uint32_t>(hp.size()),128*MiB,g.data(),32)==0,"geometry");
    const auto hidden=digest(base,g[2],g[3]);
    for(unsigned rep=1;rep<=2;++rep) {
      Mounted m;m.path=output/(L"sequential-"+std::to_wstring(rep)+L".hc");need(fs::copy_file(base,m.path,fs::copy_options::none),"copy base");choose_drive(m);m.open();
      auto record=[&](unsigned file,const char* stage,bool ok,DWORD error=0,std::uint64_t value=0,std::uint64_t lcn=0) {
        log<<rep<<','<<file<<','<<stage<<','<<ok<<','<<error<<','<<value<<','<<lcn<<','<<(m.active&&m.type()==volume::VolumeType::outer_write_prevented)<<std::endl;
        std::cout<<rep<<'/'<<file<<' '<<stage<<" ok="<<ok<<" error="<<error<<std::endl;
      };
      const auto l=layout(m.drive,g[2]-g[0]);need(l.fat==32&&l.cluster==1024,"expected FAT32 fixture");
      for(unsigned i=0;i<48;++i)seed_file(m.root()+L"s"+std::to_wstring(i)+L".bin");
      if(std::wstring_view(argv[1])==L"--production-write-run") {
        for(const std::uint64_t smallSize: {0ULL, 513ULL, 65537ULL}) {
          const auto identity=m.provider.mount_identity(m.drive);auto volumeHandle=raw(m.drive);
          const auto plan=volume::plan_fat32_allocation(volumeHandle.h,g[1],g[2]-g[0],smallSize);
          const auto targetPath=m.root()+L"small.bin";const auto sourcePath=output/(L"small-"+std::to_wstring(rep)+L"-"+std::to_wstring(smallSize)+L".bin");
          {std::ofstream sourceOutput(sourcePath,std::ios::binary);for(std::uint64_t i=0;i<smallSize;++i)sourceOutput.put(static_cast<char>(placement_byte(i)));need(sourceOutput.good(),"small source fixture");}
          Handle inputFile,target;inputFile.h=::CreateFileW(sourcePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
          target.h=::CreateFileW(targetPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
          need(inputFile.h!=INVALID_HANDLE_VALUE&&target.h!=INVALID_HANDLE_VALUE,"small handles");
          volume::prepare_fat32_file(target.h,identity,g[2]-g[0],plan);
          volume::write_prepared_fat32_file(inputFile.h,target.h,identity,g[2]-g[0],plan);
          target.close();need(fs::file_size(targetPath)==smallSize&&verify_pattern(targetPath,smallSize),"small copy mismatch");
          need(::DeleteFileW(targetPath.c_str()),"delete own small fixture");record(0,"production_small_write",true,0,smallSize);
        }
      }
      if(std::wstring_view(argv[1])==L"--production-transfer-run") {
        const auto identity=m.provider.mount_identity(m.drive);const auto beforeFiles=existing_files(m.root());
        auto volumeHandle=raw(m.drive);const auto beforeSpace=volume::scan_fat32_safe_space(volumeHandle.h,g[1],g[2]-g[0]).free_bytes;
        const auto plan=volume::plan_fat32_allocation(volumeHandle.h,g[1],g[2]-g[0],MiB);
        const auto sourcePath=output/(L"transaction-source-"+std::to_wstring(rep)+L".bin");
        {std::ofstream outFile(sourcePath,std::ios::binary);for(std::uint64_t n=0;n<MiB;++n)outFile.put(static_cast<char>(placement_byte(n)));need(outFile.good(),"transaction source");}
        Handle inputFile;inputFile.h=::CreateFileW(sourcePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);need(inputFile.h!=INVALID_HANDLE_VALUE,"transaction source handle");
        const auto path=m.root()+L"owned-transfer.bin";
        for(unsigned test=0;test<3;++test) {
          volume::Fat32TransferTarget target(path);bool cancel=false;
          const auto result=volume::transfer_fat32_file(inputFile.h,target,identity,g[2]-g[0],plan,[&]{return cancel;},[&](auto){if(test==0)cancel=true;else if(test==1)throw std::runtime_error("fixture progress failure");});
          if(test<2) {
            need(result.failure&&result.deletion_requested&&result.cleanup_error==0,"owned cancellation cleanup failed");
            bool original=false;try {std::rethrow_exception(result.failure);}catch(const std::system_error& e){original=test==0&&e.code()==std::make_error_code(std::errc::operation_canceled);}catch(const std::runtime_error& e){original=test==1&&std::string(e.what())=="fixture progress failure";}
            need(original&&!fs::exists(path),"cleanup hid original failure or left visible target");record(0,"owned_cleanup",true,0,test);
          } else {
            need(!result.failure&&verify_pattern(path,MiB),"owned transfer content mismatch");
            bool occupied=false;try {volume::Fat32TransferTarget duplicate(path);}catch(const std::system_error& e){occupied=e.code().value()==ERROR_FILE_EXISTS||e.code().value()==ERROR_ALREADY_EXISTS;}
            need(occupied&&verify_pattern(path,MiB),"occupied target changed");need(::DeleteFileW(path.c_str()),"delete own successful fixture");record(0,"owned_success_and_collision",true);
          }
          bool consumed=false;try {static_cast<void>(volume::transfer_fat32_file(inputFile.h,target,identity,g[2]-g[0],plan));}catch(const std::logic_error&){consumed=true;}need(consumed,"consumed target reused");
        }
        volumeHandle.close();m.close();m.open();
        need(unchanged_files(beforeFiles,existing_files(m.root())),"owned cleanup changed retained files");
        {auto fresh=raw(m.drive);need(volume::scan_fat32_safe_space(fresh.h,g[1],g[2]-g[0]).free_bytes==beforeSpace,"owned cleanup space not restored after remount");}
        record(0,"owned_cleanup_persisted",true);
      }
      const std::array<unsigned,5> starts{0,2,5,9,14};
      for(unsigned group=0;group<5;++group)for(unsigned j=0;j<=group;++j)need(::DeleteFileW((m.root()+L"s"+std::to_wstring(starts[group]+j)+L".bin").c_str()),"delete own fixture seed");
      m.close();m.open(); // Persist the fixture before recording the starting bitmap.
      auto mountIdentity=m.provider.mount_identity(m.drive);
      need(mountIdentity.protected_outer_writable&&mountIdentity.disk_length==g[1],"protected mount identity mismatch");
      auto ordered=[&]() {auto runs=safe_free_runs(m,l);std::sort(runs.begin(),runs.end());return runs;};
      const auto initial=ordered();need(initial.size()>=5,"missing five holes");
      for(unsigned i=0;i<5;++i) {record(0,"initial_hole",true,0,(initial[i].second-initial[i].first)*l.cluster,initial[i].first);need((initial[i].second-initial[i].first)*l.cluster==(i+1)*MiB,"five hole sizes differ from requested layout");}
      const auto originals=existing_files(m.root());need(originals.size()==33,"expected 33 retained seeds");
      auto available=[&]() {need(m.provider.mount_identity(m.drive)==mountIdentity,"mount identity changed during scan");std::uint64_t total=0;for(const auto& r:ordered())total+=(r.second-r.first)*l.cluster;auto handle=raw(m.drive);const auto formal=volume::scan_fat32_safe_space(handle.h,g[1],g[2]-g[0]);need(formal.free_bytes==total&&formal.cluster_size==l.cluster,"production safe-space mismatch");return total;};
      record(0,"safe_free_before",true,0,available());
      std::vector<std::pair<std::wstring,std::uint64_t>> writtenFiles;
      auto put=[&](unsigned id,std::uint64_t size,const Extents* exact) {
        const auto name=L"new"+std::to_wstring(id)+L".bin",path=m.root()+name;
        const auto before=available();record(id,"available_before",true,0,before);
        if(size>before) {need(!fs::exists(path),"rejected file exists");record(id,"rejected_before_create",true,0,size);return false;}
        need(size>0&&size%l.cluster==0,"aligned nonempty upload required");
        Extents planned;std::uint64_t needed=size/l.cluster;
        for(const auto& run:ordered()) {if(!needed)break;const auto count=std::min(needed,run.second-run.first);planned.emplace_back(run.first,run.first+count);needed-=count;}
        need(!needed,"insufficient preallocation plan");
        volume::Fat32AllocationPlan formal;
        {auto handle=raw(m.drive);formal=volume::plan_fat32_allocation(handle.h,g[1],g[2]-g[0],size);
          Extents actual;for(const auto& run:formal.runs)actual.emplace_back(run.first_cluster,run.first_cluster+run.cluster_count);
          need(actual==planned&&formal.file_bytes==size&&formal.allocation_bytes==size&&formal.cluster_size==l.cluster,"production first-fit plan mismatch");}
        Handle f;f.h=::CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);need(f.h!=INVALID_HANDLE_VALUE,"create sequential file");
        auto vol=raw(m.drive); Extents checked;
        if(std::wstring_view(argv[1])==L"--production-placement-run" || std::wstring_view(argv[1])==L"--production-write-run") {
          auto rejectBeforeAllocation=[&](auto action) {
            bool rejected=false;try { action(); } catch(const std::exception&) { rejected=true; }
            need(rejected,"invalid placement accepted");
            FILE_STANDARD_INFO info{};need(::GetFileInformationByHandleEx(f.h,FileStandardInfo,&info,sizeof(info))&&info.EndOfFile.QuadPart==0&&info.AllocationSize.QuadPart==0,"rejected placement allocated file");
          };
          rejectBeforeAllocation([&]{volume::prepare_fat32_file(f.h,mountIdentity,g[2]-g[0],formal,[]{return true;});});
          auto stale=mountIdentity;++stale.unique_id;
          rejectBeforeAllocation([&]{volume::prepare_fat32_file(f.h,stale,g[2]-g[0],formal);});
          auto invalid=formal;++invalid.allocation_bytes;
          rejectBeforeAllocation([&]{volume::prepare_fat32_file(f.h,mountIdentity,g[2]-g[0],invalid);});
          invalid=formal;invalid.runs.front().first_cluster=l.clusters;
          rejectBeforeAllocation([&]{volume::prepare_fat32_file(f.h,mountIdentity,g[2]-g[0],invalid);});
          record(id,"production_rejections",true);
          volume::prepare_fat32_file(f.h,mountIdentity,g[2]-g[0],formal);
          record(id,"production_prepare",true);
        } else {
        LARGE_INTEGER eof{};eof.QuadPart=static_cast<LONGLONG>(size);need(::SetFilePointerEx(f.h,eof,nullptr,FILE_BEGIN),"seek eof");
        const BOOL allocated=::SetEndOfFile(f.h);record(id,"preallocate",allocated!=FALSE,allocated?0 : ::GetLastError());need(allocated&&m.type()==volume::VolumeType::outer,"preallocate failed");
        const auto allocation=volume::read_fat32_file_allocation(f.h,size,static_cast<std::uint32_t>(l.cluster),l.clusters);
        checked.clear();for(const auto& run:allocation)checked.emplace_back(run.first_cluster,run.first_cluster+run.cluster_count);
        need(checked==extent_ranges(f.h),"production preallocation inspection mismatch");
        std::uint64_t vcn=0;
        for(const auto& destination:planned) {
          std::uint64_t first=destination.first;
          while(first<destination.second) {
            const auto current=extent_ranges(f.h);std::uint64_t baseVcn=0,sourceLcn=0,sourceCount=0;
            for(const auto& extent:current) {
              const auto length=extent.second-extent.first;
              if(vcn<baseVcn+length){sourceLcn=extent.first+vcn-baseVcn;sourceCount=baseVcn+length-vcn;break;}baseVcn+=length;
            }
            need(sourceCount>0,"source VCN absent");
            auto count=std::min(destination.second-first,sourceCount);
            if(sourceLcn==first)record(id,"retain_part",true,0,count*l.cluster,first);
            else {
              const auto fresh=ordered();std::uint64_t freeCount=0;
              for(const auto& run:fresh)if(run.first<=first&&first<run.second){freeCount=run.second-first;break;}
              need(freeCount>0,"planned destination occupied by another extent or writer");count=std::min(count,freeCount);
              MOVE_FILE_DATA move{};move.FileHandle=f.h;move.StartingVcn.QuadPart=static_cast<LONGLONG>(vcn);move.StartingLcn.QuadPart=static_cast<LONGLONG>(first);move.ClusterCount=static_cast<DWORD>(count);DWORD bytes=0;
              const BOOL moved=::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&move,sizeof(move),nullptr,0,&bytes,nullptr);
              record(id,"move_part",moved!=FALSE,moved?0 : ::GetLastError(),count*l.cluster,first);need(moved&&m.type()==volume::VolumeType::outer,"sequential move failed");
            }
            vcn+=count;first+=count;
          }
        }
        }
        need(extent_ranges(f.h)==planned,"actual extents mismatch");
        checked.clear();for(const auto& run:volume::read_fat32_file_allocation(f.h,size,static_cast<std::uint32_t>(l.cluster),l.clusters))
          checked.emplace_back(run.first_cluster,run.first_cluster+run.cluster_count);
        need(checked==planned,"production final allocation mismatch");
        if(exact)need(planned==*exact,"first fit does not match expected remaining holes");
        for(const auto& range:planned)need(l.data+range.second*l.cluster<=l.boundary,"unsafe extent");
        record(id,"extents_verified",true,0,planned.size());
        LARGE_INTEGER zero{};need(::SetFilePointerEx(f.h,zero,nullptr,FILE_BEGIN),"rewind");
        std::vector<unsigned char> content(static_cast<size_t>(size));for(size_t i=0;i<content.size();++i)content[i]=placement_byte(i);
        if(std::wstring_view(argv[1])==L"--production-write-run") {
          const auto sourcePath=output/(L"source-"+std::to_wstring(rep)+L"-"+std::to_wstring(id)+L".bin");
          {std::ofstream sourceOutput(sourcePath,std::ios::binary);sourceOutput.write(reinterpret_cast<const char*>(content.data()),static_cast<std::streamsize>(content.size()));need(sourceOutput.good(),"create source fixture");}
          Handle inputFile;inputFile.h=::CreateFileW(sourcePath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);need(inputFile.h!=INVALID_HANDLE_VALUE,"open copy source");
          bool stopped=false,cancel=false;std::uint64_t progress=0;
          try {volume::write_prepared_fat32_file(inputFile.h,f.h,mountIdentity,g[2]-g[0],formal,[&]{return cancel;},[&](auto done){progress=done;cancel=true;});}
          catch(const std::system_error& error){stopped=error.code()==std::make_error_code(std::errc::operation_canceled);}
          need(stopped&&progress==65536,"mid-copy cancellation failed");record(id,"production_cancel",true,0,progress);
          bool sameRejected=false;try {volume::write_prepared_fat32_file(f.h,f.h,mountIdentity,g[2]-g[0],formal);}catch(const std::invalid_argument&){sameRejected=true;}
          need(sameRejected,"self-copy accepted");
          progress=0;volume::write_prepared_fat32_file(inputFile.h,f.h,mountIdentity,g[2]-g[0],formal,{},[&](auto done){need(done>progress&&done<=size,"invalid progress");progress=done;});
          need(progress==size,"incomplete copy progress");record(id,"production_write",true,0,progress);
        } else {
        DWORD bytes=0;const BOOL wrote=::WriteFile(f.h,content.data(),static_cast<DWORD>(size),&bytes,nullptr);record(id,"write",wrote!=FALSE,wrote?0 : ::GetLastError(),bytes);need(wrote&&bytes==size&&::FlushFileBuffers(f.h),"write flush failed");
        }
        f.close();vol.close();need(verify_pattern(path,size),"immediate content mismatch");writtenFiles.emplace_back(name,size);
        need(m.type()==volume::VolumeType::outer,"unexpected protection trigger");
        record(id,"safe_free_after",true,0,available());
        if(rep==2){m.close();m.open();const auto nextIdentity=m.provider.mount_identity(m.drive);need(nextIdentity.unique_id!=mountIdentity.unique_id&&nextIdentity.protected_outer_writable,"remount identity not refreshed");mountIdentity=nextIdentity;for(const auto& item:writtenFiles)need(verify_pattern(m.root()+item.first,item.second),"between-files persistence");record(id,"between_files_remount",true);}
        return true;
      };
      const auto units=MiB/l.cluster;
      Extents first{initial[0],initial[1],initial[2],{initial[3].first,initial[3].first+2*units}};
      Extents second{{initial[3].first+2*units,initial[3].second},{initial[4].first,initial[4].first+units}};
      Extents third{{initial[4].first+units,initial[4].second}};
      need(put(1,8*MiB,&first)&&put(2,3*MiB,&second)&&put(3,4*MiB,&third),"example upload rejected");
      auto beforeReject=existing_files(m.root());const auto remaining=available();
      need(!put(4,remaining+l.cluster,nullptr),"oversized file accepted");need(unchanged_files(beforeReject,existing_files(m.root()))&&available()==remaining,"rejection changed files or space");record(4,"reject_unchanged",true);
      if(remaining)need(put(5,remaining,nullptr),"remaining space upload rejected");
      need(available()==0,"safe space not fully consumed");record(5,"safe_space_exhausted",true);
      beforeReject=existing_files(m.root());need(!put(6,4096,nullptr),"full volume upload accepted");need(unchanged_files(beforeReject,existing_files(m.root()))&&available()==0,"full rejection mutated volume");record(6,"reject_unchanged",true);
      m.close();m.open();for(const auto& item:writtenFiles)need(verify_pattern(m.root()+item.first,item.second),"final persistence");record(0,"all_new_files_persisted",true,0,writtenFiles.size());
      for(const auto& original:originals)need(fs::file_size(fs::path(m.root())/original.name)==original.size&&digest(fs::path(m.root())/original.name,0,original.size)==original.hash,"original seed changed");
      need(existing_files(m.root()).size()==originals.size()+writtenFiles.size(),"unexpected file set");record(0,"original_files_unchanged",true,0,originals.size());
      need(m.type()==volume::VolumeType::outer,"protection triggered before close");need(!m.close(),"protection trigger at unmount");need(digest(m.path,g[2],g[3])==hidden,"hidden region changed");record(0,"hidden_unchanged",true);
    }
    std::ofstream(output/L"complete.txt")<<"PASS two sequential first-fit trials, exact five-hole placement, leftovers, exhaustion and safe rejection verified."<<std::endl;return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<std::endl;return 1;}
}

int abrupt_test_writer(wchar_t* value) {
  // Only the test parent's inherited, already-open partial-file handle is used.
  wchar_t* end=nullptr;const auto numeric=std::wcstoull(value,&end,10);
  if(!end||*end||!numeric)return 97;
  const auto file=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(numeric));
  LARGE_INTEGER length{};wchar_t path[32768]{};
  if(::GetFileType(file)!=FILE_TYPE_DISK||!::GetFileSizeEx(file,&length)||length.QuadPart!=8*MiB||
     !::GetFinalPathNameByHandleW(file,path,32768,FILE_NAME_NORMALIZED)||fs::path(path).filename()!=L"partial.bin")return 97;
  ::TerminateProcess(::GetCurrentProcess(),99);return 98;
}
int fault_run(wchar_t** argv) {
  bool created=false;
  try {
    const fs::path dll=argv[2],source=argv[3],output=argv[4];
    need(dll.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"fault run requires new absolute output");
    const auto base=source/L"triggered-1.hc";need(fs::is_regular_file(source/L"complete.txt")&&fs::file_size(base)==128*MiB,"completed recovery fixture required");
    need(fs::create_directory(output),"create fault output");created=true;
    std::ofstream log(output/L"results.csv");log<<"case,stage,ok,error,value,triggered"<<std::endl;
    Backend backend;backend.module=::LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);need(backend.module!=nullptr,"load DLL");need(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")()==0,"initialize DLL");backend.shutdown=backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    std::ifstream headers(base,std::ios::binary);std::array<unsigned char,512>a{},b{};headers.read(reinterpret_cast<char*>(a.data()),512);headers.seekg(65536);headers.read(reinterpret_cast<char*>(b.data()),512);need(headers.good(),"headers");headers.close();
    const std::string op="DisposableOuterP3259",hp="DisposableHiddenP3259";std::array<std::uint64_t,4>g{};
    need(backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry")(a.data(),512,b.data(),512,reinterpret_cast<const unsigned char*>(op.data()),static_cast<std::uint32_t>(op.size()),reinterpret_cast<const unsigned char*>(hp.data()),static_cast<std::uint32_t>(hp.size()),128*MiB,g.data(),32)==0,"geometry");
    const auto hidden=digest(base,g[2],g[3]);
    for(unsigned scenario=0;scenario<6;++scenario) {
      Mounted m;m.path=output/(L"fault-"+std::to_wstring(scenario)+L".hc");need(fs::copy_file(base,m.path,fs::copy_options::none),"copy fault fixture");choose_drive(m);m.open();
      auto record=[&](const char* stage,bool ok,DWORD error=0,std::uint64_t value=0) {log<<scenario<<','<<stage<<','<<ok<<','<<error<<','<<value<<','<<(m.active&&m.type()==volume::VolumeType::outer_write_prevented)<<std::endl;};
      const auto l=layout(m.drive,g[2]-g[0]);const auto original=existing_files(m.root());
      auto total=[&](){std::uint64_t count=0;for(const auto& r:safe_free_runs(m,l))count+=(r.second-r.first)*l.cluster;return count;};
      const auto before=total();record("free_before",true,0,before);
      const auto path=m.root()+L"partial.bin",rivalPath=m.root()+L"rival.bin";
      Handle f;f.h=::CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);need(f.h!=INVALID_HANDLE_VALUE,"new partial file");
      if(scenario==0) {f.close();f.h=::CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);need(f.h!=INVALID_HANDLE_VALUE,"read-only reopen");}
      LARGE_INTEGER end{};end.QuadPart=8*MiB;need(::SetFilePointerEx(f.h,end,nullptr,FILE_BEGIN),"seek EOF");
      const BOOL allocated=::SetEndOfFile(f.h);const DWORD allocationError=allocated?0 : ::GetLastError();record("preallocate",allocated!=FALSE,allocationError);
      if(scenario==0)need(!allocated&&allocationError==ERROR_ACCESS_DENIED,"allocation failure injection did not fire");
      else {
        need(allocated,"preallocate");auto vol=raw(m.drive);std::uint64_t vcn=0;
        const auto parts=scenario==1||scenario==5?2U:scenario==2?0U:scenario==4?0U:8U;
        for(unsigned part=0;part<parts;++part) {
          auto runs=safe_free_runs(m,l);need(!runs.empty()&&runs[0].second-runs[0].first>=MiB/l.cluster,"missing one MiB destination");
          MOVE_FILE_DATA move{};move.FileHandle=f.h;move.StartingVcn.QuadPart=static_cast<LONGLONG>(vcn);move.StartingLcn.QuadPart=static_cast<LONGLONG>(runs[0].first);move.ClusterCount=static_cast<DWORD>(MiB/l.cluster);DWORD bytes=0;
          need(::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&move,sizeof(move),nullptr,0,&bytes,nullptr),"move safe part");vcn+=move.ClusterCount;record("moved_part",true,0,vcn*l.cluster);
        }
        if(scenario==1)record("cancel_after_two_parts",true);
        if(scenario==2) {
          auto runs=safe_free_runs(m,l);need(!runs.empty(),"rival target");
          Handle rival;rival.h=::CreateFileW(rivalPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);need(rival.h!=INVALID_HANDLE_VALUE,"rival create");
          LARGE_INTEGER e{};e.QuadPart=MiB;need(::SetFilePointerEx(rival.h,e,nullptr,FILE_BEGIN)&&::SetEndOfFile(rival.h),"rival allocation");
          MOVE_FILE_DATA move{};move.FileHandle=rival.h;move.StartingLcn.QuadPart=static_cast<LONGLONG>(runs[0].first);move.ClusterCount=static_cast<DWORD>(MiB/l.cluster);DWORD bytes=0;
          need(::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&move,sizeof(move),nullptr,0,&bytes,nullptr),"occupy destination");
          LARGE_INTEGER zero{};need(::SetFilePointerEx(rival.h,zero,nullptr,FILE_BEGIN),"rival rewind");std::vector<unsigned char> data(MiB,0x75);need(::WriteFile(rival.h,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr)&&bytes==data.size()&&::FlushFileBuffers(rival.h),"rival write");
          move.FileHandle=f.h;const BOOL moved=::DeviceIoControl(vol.h,FSCTL_MOVE_FILE,&move,sizeof(move),nullptr,0,&bytes,nullptr);const DWORD error=moved?0 : ::GetLastError();record("occupied_destination_rejected",!moved,error);need(!moved,"occupied target overwritten");
          rival.close();need(verify_upload(rivalPath,MiB),"rival contents damaged");record("rival_unchanged",true);
        }
        if(scenario==3||scenario==4) {
          if(scenario==3){const auto extents=extent_ranges(f.h);for(const auto& r:extents)need(l.data+r.second*l.cluster<=l.boundary,"unsafe prefix target");}
          LARGE_INTEGER zero{};need(::SetFilePointerEx(f.h,zero,nullptr,FILE_BEGIN),"prefix rewind");std::vector<unsigned char> data(65536,0x75);DWORD bytes=0;
          const BOOL wrote=::WriteFile(f.h,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr);const DWORD error=wrote?0 : ::GetLastError();record("prefix_write",wrote!=FALSE,error,bytes);
          if(scenario==4)need(!wrote&&m.type()==volume::VolumeType::outer_write_prevented,"real protection failure not reproduced");
          else {need(wrote&&bytes==data.size()&&::FlushFileBuffers(f.h),"prefix write");f.close();f.h=::CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);need(f.h!=INVALID_HANDLE_VALUE,"read-only write injection");const BOOL denied=::WriteFile(f.h,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr);const DWORD deniedError=denied?0 : ::GetLastError();record("write_failure_injected",!denied,deniedError);need(!denied&&deniedError==ERROR_ACCESS_DENIED,"write injection failed");}
        }
        if(scenario==5) {
          need(::SetHandleInformation(f.h,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"inherit partial handle");wchar_t exe[32768]{};need(::GetModuleFileNameW(nullptr,exe,32768)>0,"self path");std::wstring command=L"\""+std::wstring(exe)+L"\" --abrupt-disposable-writer "+std::to_wstring(reinterpret_cast<std::uintptr_t>(f.h));
          STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};need(::CreateProcessW(exe,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process),"start abrupt child");
          f.close();Handle child,thread;child.h=process.hProcess;thread.h=process.hThread;
          if(::WaitForSingleObject(child.h,30000)!=WAIT_OBJECT_0){::TerminateProcess(child.h,100);::WaitForSingleObject(child.h,5000);throw std::runtime_error("abrupt child timed out");}
          DWORD code=0;need(::GetExitCodeProcess(child.h,&code)&&code==99,"child did not terminate at injected point");record("abrupt_process_exit",true,0,code);
        }
      }
      f.close();
      if(scenario==5){m.close();m.open();need(fs::is_regular_file(path),"interrupted partial file unexpectedly absent");record("partial_survives_process_exit",true,0,fs::file_size(path));}
      const BOOL removed=::DeleteFileW(path.c_str());const DWORD cleanupError=removed?0 : ::GetLastError();record("cleanup_attempt",removed!=FALSE,cleanupError);
      if(scenario!=4)need(removed,"cleanup should succeed on writable test volume");
      m.close();m.open();
      if(fs::exists(path)){need(::DeleteFileW(path.c_str()),"retry cleanup after remount");record("cleanup_after_remount",true);}
      if(scenario==2){need(verify_upload(rivalPath,MiB),"rival persistence");need(::DeleteFileW(rivalPath.c_str()),"remove own rival fixture");}
      m.close();m.open();need(unchanged_files(original,existing_files(m.root())),"original files or file set changed");record("originals_unchanged",true,0,original.size());
      const auto after=total();record("free_after",after==before,0,after);need(after==before,"safe space leaked after cleanup");need(m.type()==volume::VolumeType::outer,"protection latch persisted across remount");m.close();need(digest(m.path,g[2],g[3])==hidden,"hidden region changed");record("hidden_unchanged",true);
    }
    std::ofstream(output/L"complete.txt")<<"PASS six failure scenarios; original files, reclaimed space and hidden hashes verified."<<std::endl;return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<std::endl;return 1;}
}


int measurement_helper_run(wchar_t** argv) {
  bool created=false;
  try {
    const fs::path helper=argv[2],source=argv[3],output=argv[4],base=source/L"base-128.hc";
    need(helper.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"new absolute helper run required");
    need(fs::is_regular_file(source/L"complete.txt")&&fs::file_size(base)==128*MiB,"disposable base required");
    need(fs::create_directory(output),"create measurement output");created=true;
    Mounted m;m.path=output/L"measurement.hc";
    need(fs::copy_file(base,m.path,fs::copy_options::none),"copy measurement fixture");
    const auto other=output/L"other.hc";need(fs::copy_file(base,other,fs::copy_options::none),"copy other identity");
    const auto hidden=digest(base,66977792,66977792),otherHash=digest(other,0,128*MiB);
    choose_drive(m);m.open();
    auto identity=m.provider.mount_identity(m.drive);
    need(identity.protected_outer_writable,"protected identity required");
    auto password=[](std::string_view text) {security::SecureBuffer p(text.size());std::memcpy(p.data(),text.data(),text.size());return p;};
    const auto outer=password("DisposableOuterP3259"),inner=password("DisposableHiddenP3259"),wrong=password("WrongDisposableP3259");
    usb_vault::volume_creation::VolumeMeasurementSession session(helper);
    auto measure=[&]() {return session.protected_space(m.path.native(),m.drive,identity.unique_id,identity.disk_length,outer,inner);};
    auto rejects=[&](auto action) {bool failed=false;try{static_cast<void>(action());}catch(const std::system_error&){failed=true;}catch(const std::invalid_argument&){failed=true;}need(failed&&session.open(),"bad request accepted or healthy pipe closed");};
    const auto before=measure();need(before>MiB&&before<64*MiB,"unexpected safe capacity");
    rejects([&]{return session.protected_space(other.native(),m.drive,identity.unique_id,identity.disk_length,outer,inner);});
    rejects([&]{return session.protected_space(m.path.native(),m.drive,identity.unique_id+1,identity.disk_length,outer,inner);});
    rejects([&]{return session.protected_space(m.path.native(),m.drive,identity.unique_id,identity.disk_length+512,outer,inner);});
    rejects([&]{return session.protected_space(m.path.native(),m.drive,identity.unique_id,identity.disk_length,wrong,inner);});
    rejects([&]{return session.protected_space(m.path.native(),m.drive,identity.unique_id,identity.disk_length,outer,wrong);});
    rejects([&]{return session.protected_space(m.path.native()+L":stream",m.drive,identity.unique_id,identity.disk_length,outer,inner);});
    need(measure()==before,"failure changed measurement");
    seed_file(m.root()+L"seed.bin");need(measure()==before-MiB,"write did not refresh safe capacity");
    m.close();m.open();rejects(measure);
    identity=m.provider.mount_identity(m.drive);
    need(measure()==before-MiB,"remount measurement differs");
    session.close();need(!session.open(),"session stayed open");
    need(!m.close(),"unexpected protection trigger");
    need(digest(m.path,66977792,66977792)==hidden&&digest(other,0,128*MiB)==otherHash,"readonly helper damaged fixtures");
    std::ofstream(output/L"complete.txt")<<"PASS bound helper measurement, six rejection cases, failure reuse, write refresh, stale remount rejection and hidden integrity.\n";
    return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<std::endl;if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<std::endl;return 1;}
}

int import_helper_run(wchar_t** argv) {
  bool created=false;
  try {
    const fs::path helper=argv[2],source=argv[3],output=argv[4],base=source/L"base-128.hc";
    need(helper.is_absolute()&&source.is_absolute()&&output.is_absolute()&&!fs::exists(output)&&fs::is_directory(output.parent_path()),"new import fixture required");
    need(fs::is_regular_file(source/L"complete.txt")&&fs::file_size(base)==128*MiB,"disposable base required");
    need(fs::create_directory(output),"create import output");created=true;
    Mounted m;m.path=output/L"import.hc";need(fs::copy_file(base,m.path),"copy import fixture");choose_drive(m);m.open();
    const auto hidden=digest(base,66977792,66977792);auto identity=m.provider.mount_identity(m.drive);
    const auto payload=output/L"payload.bin";{std::ofstream data(payload,std::ios::binary);for(std::uint64_t i=0;i<8*MiB+513;++i)data.put(static_cast<char>(placement_byte(i)));need(data.good(),"create import payload");}
    auto outer=secret("DisposableOuterP3259"),inner=secret("DisposableHiddenP3259"),wrong=secret("WrongDisposableP3259");
    auto invoke=[&](std::wstring_view destination,std::int32_t id,const security::SecureBuffer& key,const volume_creation::ProgressCallback& progress=volume_creation::ProgressCallback{},const volume_creation::CancellationQuery& cancelled=volume_creation::CancellationQuery{}) {
      volume_creation::import_protected_file(helper,m.path.native(),m.drive,id,identity.disk_length,outer,key,payload.native(),destination,progress,cancelled);
    };
    auto rejects=[&](auto action){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}need(rejected,"invalid import accepted");};
    rejects([&]{invoke(L"../outside.bin",identity.unique_id,inner);});
    rejects([&]{invoke(L"bad.bin",identity.unique_id+1,inner);});need(!fs::exists(m.root()+L"bad.bin"),"stale import created target");
    rejects([&]{invoke(L"bad.bin",identity.unique_id,wrong);});need(!fs::exists(m.root()+L"bad.bin"),"bad credentials created target");
    need(::CreateDirectoryW((m.root()+L"folder").c_str(),nullptr),"create own test directory");
    invoke(L"folder/copied.bin",identity.unique_id,inner);need(verify_pattern(m.root()+L"folder\\copied.bin",8*MiB+513),"helper copied payload differs");
    rejects([&]{invoke(L"folder/copied.bin",identity.unique_id,inner);});need(verify_pattern(m.root()+L"folder\\copied.bin",8*MiB+513),"collision changed payload");
    volume_creation::VolumeImportSession uploads(helper);
    const auto persistent=[&](std::wstring_view name,std::int32_t mountId) {
      uploads.copy(m.path.native(),m.drive,mountId,identity.disk_length,outer,inner,payload.native(),name);
    };
    persistent(L"session-one.bin",identity.unique_id);need(uploads.open(),"upload session closed after success");
    persistent(L"session-two.bin",identity.unique_id);need(uploads.open()&&verify_pattern(m.root()+L"session-two.bin",8*MiB+513),"second session copy failed");
    rejects([&]{persistent(L"stale.bin",identity.unique_id+1);});need(!uploads.open()&&!fs::exists(m.root()+L"stale.bin"),"session switched mounts");
    rejects([&]{persistent(L"latched.bin",identity.unique_id);});need(!uploads.open()&&!fs::exists(m.root()+L"latched.bin"),"failed upload session restarted");
    uploads.close();
    bool cancel=false,cancelled=false;try {invoke(L"cancel.bin",identity.unique_id,inner,[&](const auto& progress){if(progress.percent>=1)cancel=true;},[&]{return cancel;});}
    catch(const std::system_error& error){cancelled=error.code()==volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled);}
    need(cancelled&&!fs::exists(m.root()+L"cancel.bin"),"helper cancellation cleanup failed");
    const auto tree=output/L"tree-source";
    need(fs::create_directories(tree/L"nested"/L"empty"),"tree fixture directories");
    need(fs::copy_file(payload,tree/L"one.bin")&&fs::copy_file(payload,tree/L"nested"/L"two.bin"),"tree fixture payloads");
    {std::ofstream empty(tree/L"zero.bin",std::ios::binary);need(empty.good(),"empty tree file");}
    const auto treeCopy=[&](std::wstring_view name,const volume_creation::ProgressCallback& progress=volume_creation::ProgressCallback{},const volume_creation::CancellationQuery& stop=volume_creation::CancellationQuery{}) {
      volume_creation::import_protected_file(helper,m.path.native(),m.drive,identity.unique_id,identity.disk_length,outer,inner,tree.native(),name,progress,stop);
    };
    treeCopy(L"tree");need(verify_pattern(m.root()+L"tree\\one.bin",8*MiB+513)&&verify_pattern(m.root()+L"tree\\nested\\two.bin",8*MiB+513)&&fs::is_directory(m.root()+L"tree\\nested\\empty")&&fs::file_size(m.root()+L"tree\\zero.bin")==0,"tree copy differs");
    rejects([&]{treeCopy(L"tree");});need(verify_pattern(m.root()+L"tree\\one.bin",8*MiB+513),"tree collision changed old content");
    bool stopTree=false,treeCancelled=false;std::string treeError;unsigned treePercent=0;
    try {treeCopy(L"tree-cancel",[&](const auto& p){treePercent=p.percent;if(p.percent>=51)stopTree=true;},[&]{return stopTree;});}
    catch(const std::system_error& e){treeError=e.what();treeCancelled=e.code()==volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled);}
    if(!treeCancelled||fs::exists(m.root()+L"tree-cancel"))throw std::runtime_error("tree rollback failed: percent="+std::to_string(treePercent)+" cancelled="+std::to_string(treeCancelled)+" exists="+std::to_string(fs::exists(m.root()+L"tree-cancel"))+" error="+treeError);
    const auto crowded=output/L"crowded";need(fs::create_directory(crowded),"create bounded-source fixture");
    for(unsigned n=0;n<1024;++n){std::ofstream empty(crowded/(std::to_wstring(n)+L".bin"));need(empty.good(),"bounded source entry");}
    rejects([&]{volume_creation::import_protected_file(helper,m.path.native(),m.drive,identity.unique_id,identity.disk_length,outer,inner,crowded.native(),L"crowded");});
    need(!fs::exists(m.root()+L"crowded"),"over-limit tree created destination");
    need(!m.close(),"helper triggered protection");m.open();need(!fs::exists(m.root()+L"tree-cancel")&&!fs::exists(m.root()+L"cancel.bin")&&verify_pattern(m.root()+L"folder\\copied.bin",8*MiB+513),"helper remount persistence failed");
    need(!m.close()&&digest(m.path,66977792,66977792)==hidden,"helper hidden integrity failed");
    std::ofstream(output/L"complete.txt")<<"PASS medium-client protected import, traversal/stale identity/wrong password/collision rejection, cancellation cleanup, remount and hidden integrity.\n";return 0;
  } catch(const std::exception& e){if(created)std::ofstream(fs::path(argv[4])/L"failure.txt")<<e.what()<<std::endl;return 1;}
}
