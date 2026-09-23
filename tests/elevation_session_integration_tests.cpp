#include "volume_creation/elevation_session.h"
#include "volume_creation/creator_protocol.h"
#include <Windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace usb_vault::volume_creation;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
struct Handle{HANDLE h;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)::CloseHandle(h);}};
int wmain(int argc,wchar_t** argv){
 if(argc!=3||std::wstring_view(argv[1])!=L"--run-session"){std::cout<<"SKIP: explicit session test required\n";return 77;}
 try{
  const auto helper=std::filesystem::absolute(argv[2]);
  require(begin_elevation_session(helper),"startup elevation session failed");
  DWORD broker_pid{},previous{};
  for(int run=0;run<2;++run){
   const std::wstring nonce(32,run?L'b':L'a');
   Handle process{detail::launch_session_helper(helper,nonce)};
   const DWORD pid=::GetProcessId(process.h);require(pid&&pid!=previous,"fresh child was not created");previous=pid;
   Handle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
   if(::Process32FirstW(snapshot.h,&entry))do{if(entry.th32ProcessID==pid){if(broker_pid)require(broker_pid==entry.th32ParentProcessID,"broker changed between operations");broker_pid=entry.th32ParentProcessID;break;}}while(::Process32NextW(snapshot.h,&entry));
   const auto pipe_name=L"\\\\.\\pipe\\usb-vault-create-"+nonce;HANDLE pipe=INVALID_HANDLE_VALUE;
   const auto start=::GetTickCount64();
   while(::GetTickCount64()-start<10000){pipe=::CreateFileW(pipe_name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IMPERSONATION,nullptr);if(pipe!=INVALID_HANDLE_VALUE)break;::Sleep(20);}
   Handle connected{pipe};require(pipe!=INVALID_HANDLE_VALUE,"session child pipe unavailable");
   ULONG server{};require(::GetNamedPipeServerProcessId(pipe,&server)&&server==pid,"child pipe identity mismatch");
   detail::RequestHeader request;request.magic=0;DWORD written{};
   require(::WriteFile(pipe,&request,sizeof(request),&written,nullptr)&&written==sizeof(request),"malformed request send failed");
   const auto response=detail::read_response(pipe);
   require(response.error!=0,"malformed request accepted");
   require(::WaitForSingleObject(process.h,10000)==WAIT_OBJECT_0,"child did not exit after rejected request");
  }
  const auto output=helper.parent_path()/(L"session-fixture-"+std::to_wstring(::GetCurrentProcessId())+L"-"+std::to_wstring(::GetTickCount64())+L".hc");
  require(!std::filesystem::exists(output),"fixture already exists");
  usb_vault::security::SecureBuffer password(64);
  std::fill(password.bytes().begin(),password.bytes().end(),std::byte{0x63});
  VolumeCreator creator(helper);
  creator.create_standard(output.wstring(),32ULL*1024*1024,password);
  require(std::filesystem::file_size(output)==32ULL*1024*1024,"session container size mismatch");
  require(::DeleteFileW(output.c_str())!=FALSE,"owned file cleanup failed");
  require(broker_pid&&broker_pid!=::GetCurrentProcessId(),"operations did not share a broker");
  Handle broker{::OpenProcess(SYNCHRONIZE,FALSE,broker_pid)};require(broker.h!=nullptr,"cannot observe broker exit");
  bool rejected=false;try{Handle bad{detail::launch_session_helper(helper,L"invalid")};}catch(...){rejected=true;}require(rejected,"invalid nonce accepted");
  end_elevation_session();
  require(::WaitForSingleObject(broker.h,5000)==WAIT_OBJECT_0,"broker outlived closed session");
  rejected=false;try{Handle bad{detail::launch_session_helper(helper,std::wstring(32,L'c'))};}catch(...){rejected=true;}require(rejected,"closed session silently fell back to UAC");
  std::cout<<"PASS: one elevation session launched two isolated helpers and created an owned file container, rejected malformed operations, bound process identities and exited without fallback\n";return 0;
 }catch(const std::exception& e){end_elevation_session();std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
