#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume/volume_provider.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include <Windows.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace usb_vault;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
security::SecureBuffer password(const char* text) {
  security::SecureBuffer result(std::strlen(text));
  std::memcpy(result.data(), text, result.size());
  return result;
}
struct File {
  HANDLE handle{INVALID_HANDLE_VALUE};
  ~File() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
  void close() { if (handle != INVALID_HANDLE_VALUE) { ::CloseHandle(handle); handle = INVALID_HANDLE_VALUE; } }
};
int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--inspect-drives") {
    volume::VolumeProvider provider;
    for (const auto& item : provider.mounted_volumes())
      std::cout << "mounted=" << static_cast<char>(item.drive_letter) << " type=" << static_cast<int>(item.type) << '\n';
    for (wchar_t letter=L'D'; letter<=L'Z'; ++letter) {
      wchar_t name[]{letter,L':',L'\0'};
      wchar_t target[1024]{};
      if (::QueryDosDeviceW(name,target,1024)) std::wcout << name << L" -> " << target << L'\n';
    }
    return 0;
  }
  const bool create = argc == 4 && std::wstring_view(argv[1]) == L"--create-disposable-fixture";
  const bool exercise = argc == 4 && std::wstring_view(argv[1]) == L"--exercise-created-fixture";
  if (!create && !exercise) {
    std::cerr << "Requires explicit disposable-fixture flag, helper path, and NEW fixture path\n";
    return 2;
  }
  const fs::path helper = fs::absolute(argv[2]);
  const fs::path fixture = fs::absolute(argv[3]);
  if ((create && fs::exists(fixture)) || (exercise && !fs::is_regular_file(fixture)) || !fs::is_directory(fixture.parent_path())) {
    std::cerr << "Refusing existing fixture or missing parent\n";
    return 2;
  }
  volume::VolumeProvider provider;
  wchar_t drive = 0;
  bool mounted = false;
  const auto unmount = [&] {
    const auto result = provider.unmount(drive, {.force=true, .notify_shell=false});
    mounted = false;
    return result;
  };
  const auto type = [&] {
    for (const auto& item : provider.mounted_volumes())
      if (item.drive_letter == drive) return item.type;
    return volume::VolumeType::unknown;
  };
  try {
    auto outer = password("DisposableOuterP3259");
    auto hidden = password("DisposableHiddenP3259");
    volume_creation::VolumeCreator creator(helper);
    if (create) creator.create_with_hidden(fixture.native(), 128ULL*1024*1024, 64ULL*1024*1024, outer, hidden);
    std::cout << (create ? "PASS created fresh disposable outer+hidden fixture\n" : "Using explicitly selected pre-created disposable fixture\n") << std::flush;
    DWORD used = ::GetLogicalDrives();
    require(used != 0, "cannot enumerate drives");
    for (const auto& item : provider.mounted_volumes()) {
      if (item.drive_letter >= L'A' && item.drive_letter <= L'Z')
        used |= 1UL << (item.drive_letter-L'A');
    }
    // Creation may transiently use Z; inspect after it finishes and use a
    // different free letter so its mount-manager cleanup cannot race this run.
    for (wchar_t letter=L'Y'; letter>=L'D'; --letter)
      if (!(used & (1UL << (letter-L'A')))) { drive=letter; break; }
    require(drive != 0, "no unused drive letter");
    std::cout << "fixture drive=" << static_cast<char>(drive) << " used_mask=" << used << '\n' << std::flush;
    const auto mount = [&](const security::SecureBuffer& key, bool protect) {
      volume::MountOptions options{.drive_letter=drive, .notify_shell=false};
      if (protect) options.hidden_volume_protection = volume::HiddenVolumeProtection{.password=&hidden};
      static_cast<void>(provider.mount_file(fixture.native(), key, options));
      mounted = true;
    };
    const std::wstring root = std::wstring(1,drive) + L":\\";
    mount(outer, false);
    require(type()==volume::VolumeType::normal, "unprotected outer must report normal");
    static_cast<void>(unmount());
    std::cout << "PASS unprotected outer reports normal\n" << std::flush;
    mount(hidden, false);
    require(type()==volume::VolumeType::hidden, "hidden type mismatch");
    File sentinel;
    sentinel.handle=::CreateFileW((root+L"p3259-sentinel.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, nullptr);
    require(sentinel.handle!=INVALID_HANDLE_VALUE,"cannot create hidden sentinel");
    const char marker[]="p3259-protected-hidden-sentinel";
    DWORD written=0;
    require(::WriteFile(sentinel.handle,marker,sizeof(marker),&written,nullptr) && written==sizeof(marker) && ::FlushFileBuffers(sentinel.handle),"cannot flush hidden sentinel");
    sentinel.close();
    static_cast<void>(unmount());
    mount(outer,true);
    require(type()==volume::VolumeType::outer,"protected outer type mismatch");
    volume_creation::VolumeMeasurementSession measurements(helper);
    const auto tail1=measurements.tail_space(drive);
    const auto tail2=measurements.tail_space(drive);
    require(tail1>0 && tail1<=128ULL*1024*1024 && tail1==tail2 && measurements.open(),"measurement unstable or out of bounds");
    measurements.close();
    require(!measurements.open(),"measurement session did not close");
    std::cout << "PASS protected outer, repeated tail_bytes=" << tail1 << " and measurement session close\n" << std::flush;
    File held;
    held.handle=::CreateFileW((root+L"p3259-held.tmp").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held.handle!=INVALID_HANDLE_VALUE,"cannot open occupancy fixture");
    bool busy=false;
    try { static_cast<void>(provider.unmount(drive,{.notify_shell=false})); mounted=false; }
    catch(const std::system_error&) { busy=true; }
    require(busy,"occupied volume unexpectedly unmounted normally");
    static_cast<void>(unmount());
    held.close();
    mount(outer,true);
    require(type()==volume::VolumeType::outer,"cannot immediately remount after forced unmount");
    std::cout << "PASS occupied unmount rejected, force unmount and immediate remount\n" << std::flush;
    File fill;
    fill.handle=::CreateFileW((root+L"p3259-fill.bin").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
    require(fill.handle!=INVALID_HANDLE_VALUE,"cannot create outer fill file");
    std::vector<unsigned char> block(1024*1024,0x5a);
    unsigned int blocks=0;
    const auto started=::GetTickCount64();
    ULONGLONG triggered=0;
    for (;blocks<129;++blocks) {
      DWORD count=0;
      const BOOL ok=::WriteFile(fill.handle,block.data(),static_cast<DWORD>(block.size()),&count,nullptr);
      const BOOL flushed=ok ? ::FlushFileBuffers(fill.handle) : FALSE;
      if(type()==volume::VolumeType::outer_write_prevented) { triggered=::GetTickCount64(); break; }
      if(!ok || !flushed || count!=block.size()) break;
    }
    for(unsigned int i=0; !triggered && i<30; ++i) {
      if(type()==volume::VolumeType::outer_write_prevented) { triggered=::GetTickCount64(); break; }
      ::Sleep(100);
    }
    require(triggered!=0,"write failure did not become protection-triggered state");
    DWORD after=0;
    require(!::WriteFile(fill.handle,block.data(),static_cast<DWORD>(block.size()),&after,nullptr),"triggered protection accepted another write");
    fill.close();
    const auto result=unmount();
    require(result.hidden_volume_protection_triggered,"unmount did not retain protection trigger flag");
    std::cout << "PASS protection triggered after blocks=" << blocks << " total_ms=" << triggered-started << ", subsequent write refused and unmount flag set\n" << std::flush;
    mount(hidden,false);
    File verify;
    verify.handle=::CreateFileW((root+L"p3259-sentinel.bin").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(verify.handle!=INVALID_HANDLE_VALUE,"hidden sentinel missing");
    char readback[sizeof(marker)]{};
    DWORD count=0;
    require(::ReadFile(verify.handle,readback,sizeof(readback),&count,nullptr) && count==sizeof(marker) && std::memcmp(marker,readback,sizeof(marker))==0,"hidden sentinel changed");
    verify.close();
    static_cast<void>(unmount());
    std::cout << "PASS hidden sentinel preserved; no card authentication or card writes\n";
    return 0;
  } catch(const std::exception& error) {
    if(mounted) { try { static_cast<void>(unmount()); } catch(...) { std::cerr << "CLEANUP FAILED: fixture still mounted\n"; } }
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
