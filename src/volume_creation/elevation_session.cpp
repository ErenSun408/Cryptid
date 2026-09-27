#include "volume_creation/elevation_session.h"
#include "volume_creation/creator_pipe_security.h"
#include "usb_vault/diagnostics/debug_log.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace usb_vault::volume_creation {
namespace {
constexpr DWORD magic = 0x31534555;
struct Request { DWORD tag{magic}; std::array<wchar_t,32> nonce{}; };
struct Reply { DWORD tag{magic}; DWORD error{}; std::uint64_t handle{}; };
static_assert(sizeof(Request)==68 && sizeof(Reply)==16);
struct Handle {
  HANDLE value{};
  explicit Handle(HANDLE h=nullptr):value(h){}
  ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)::CloseHandle(value);}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
};
bool nonce_valid(std::wstring_view n) {
  if(n.size()!=32)return false;
  for(auto c:n)if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;
  return true;
}
std::wstring pipe_name(std::wstring_view n){return L"\\\\.\\pipe\\usb-vault-session-"+std::wstring(n);}
bool write_packet(HANDLE pipe,const void* data,DWORD bytes) {
  DWORD done{};return ::WriteFile(pipe,data,bytes,&done,nullptr)&&done==bytes;
}
// No unbounded reads; idle server lifetime is bounded by the original process.
bool read_packet(HANDLE pipe,void* data,DWORD bytes,HANDLE peer,DWORD timeout) {
  const auto start=::GetTickCount64(); ULONGLONG partial{};
  for(;;){
    if(::WaitForSingleObject(peer,0)!=WAIT_TIMEOUT)return false;
    DWORD available{};if(!::PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return false;
    if(available>=bytes){DWORD done{};return ::ReadFile(pipe,data,bytes,&done,nullptr)&&done==bytes;}
    const auto now=::GetTickCount64();
    if(available&&!partial)partial=now;
    if((timeout&&now-start>=timeout)||(partial&&now-partial>=5000))return false;
    ::Sleep(20);
  }
}
struct Session {
  std::mutex mutex;
  bool requested{};
  HANDLE pipe{},process{};
  std::filesystem::path helper;
  void close() noexcept {if(pipe){::CloseHandle(pipe);pipe=nullptr;}if(process){::CloseHandle(process);process=nullptr;}}
  ~Session(){close();}
} session;
}
bool begin_elevation_session(const std::filesystem::path& verified_helper) {
  std::lock_guard lock(session.mutex);
  if(session.requested)return session.pipe!=nullptr;
  session.requested=true;
  session.helper=std::filesystem::absolute(verified_helper).lexically_normal();
  std::array<unsigned char,16> random{};
  if(::BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
  std::wstring nonce;for(auto b:random){nonce+=L"0123456789abcdef"[b>>4];nonce+=L"0123456789abcdef"[b&15];}
  const auto args=L"--session "+std::to_wstring(::GetCurrentProcessId())+L" --pipe "+nonce+L" --nonce "+nonce;
  SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
  info.lpVerb=L"runas";info.lpFile=session.helper.c_str();info.lpParameters=args.c_str();info.nShow=SW_HIDE;
  USB_VAULT_TRACE("管理员会话", "请求管理员会话", diagnostics::utf8(session.helper.native()));
  if(!::ShellExecuteExW(&info)||!info.hProcess){
    USB_VAULT_TRACE("管理员会话", "UAC 提权失败", "Win32 " + std::to_string(::GetLastError()));
    return false;
  }
  session.process=info.hProcess;
  const auto name=pipe_name(nonce);const auto start=::GetTickCount64();
  while(::GetTickCount64()-start<60000 && ::WaitForSingleObject(session.process,0)==WAIT_TIMEOUT){
    const auto p=::CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
        SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
    if(p!=INVALID_HANDLE_VALUE){session.pipe=p;break;}
    const auto error=::GetLastError();if(error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PIPE_BUSY){
      USB_VAULT_TRACE("管理员会话", "连接会话管道失败", "Win32 " + std::to_string(error));
      break;
    }
    ::Sleep(20);
  }
  ULONG server{};Reply ready{};
  if(!session.pipe||!::GetNamedPipeServerProcessId(session.pipe,&server)||server!=::GetProcessId(session.process)||
      !read_packet(session.pipe,&ready,sizeof(ready),session.process,10000)||ready.tag!=magic||ready.error||ready.handle){
    DWORD exit_code{};
    if(session.process)static_cast<void>(::GetExitCodeProcess(session.process,&exit_code));
    USB_VAULT_TRACE("管理员会话", "管理员会话未就绪",
                    "已连接管道 " + diagnostics::flag_text(session.pipe!=nullptr) + "；服务端 " +
                        std::to_string(server) + "；就绪错误 " + std::to_string(ready.error) +
                        "；助手退出码 " + std::to_string(exit_code));
    session.close();return false;
  }
  USB_VAULT_TRACE("管理员会话", "管理员会话已建立", "进程号 " + std::to_string(::GetProcessId(session.process)));
  return true;
}
void end_elevation_session() noexcept {std::lock_guard lock(session.mutex);session.close();}
HANDLE detail::launch_session_helper(const std::filesystem::path& helper,std::wstring_view nonce) {
  std::lock_guard lock(session.mutex);
  if(!session.requested)return nullptr;
  if(!session.pipe||!session.process||std::filesystem::absolute(helper).lexically_normal()!=session.helper||!nonce_valid(nonce))
    throw std::runtime_error("administrator session unavailable");
  Request request;std::copy(nonce.begin(),nonce.end(),request.nonce.begin());Reply reply{};
  if(!write_packet(session.pipe,&request,sizeof(request))||!read_packet(session.pipe,&reply,sizeof(reply),session.process,10000)||reply.tag!=magic){
    USB_VAULT_TRACE("管理员会话", "管理员会话已断开", "Win32 " + std::to_string(::GetLastError()));
    session.close();throw std::runtime_error("administrator session disconnected");
  }
  if(reply.error||!reply.handle){
    USB_VAULT_TRACE("管理员会话", "管理员会话拒绝启动助手", "错误 " + std::to_string(reply.error));
    throw std::runtime_error("administrator session launch rejected");
  }
  const auto process=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(reply.handle));
  if(!::GetProcessId(process)){::CloseHandle(process);throw std::runtime_error("invalid session child handle");}
  return process;
}
int detail::run_elevation_session(DWORD parent,std::wstring_view nonce) noexcept {
 try {
  if(!parent||!nonce_valid(nonce))return 2;
  Handle token; if(!::OpenProcessToken(::GetCurrentProcess(),TOKEN_QUERY,&token.value))return 3;
  TOKEN_ELEVATION elevated{};DWORD returned{};
  if(!::GetTokenInformation(token.value,TokenElevation,&elevated,sizeof(elevated),&returned)||!elevated.TokenIsElevated)return 3;
  Handle owner(::OpenProcess(SYNCHRONIZE|PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));
  if(!owner.value)return 3;
  auto descriptor=current_user_pipe_security_descriptor();if(!descriptor.get())return 3;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor.get(),FALSE};
  const auto name=pipe_name(nonce);
  Handle pipe(::CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,&security));
  if(pipe.value==INVALID_HANDLE_VALUE)return 3;
  const auto start=::GetTickCount64();bool connected=false;
  while(::WaitForSingleObject(owner.value,0)==WAIT_TIMEOUT && ::GetTickCount64()-start<60000){
    if(::ConnectNamedPipe(pipe.value,nullptr)||::GetLastError()==ERROR_PIPE_CONNECTED){connected=true;break;}
    if(::GetLastError()!=ERROR_PIPE_LISTENING)return 3;::Sleep(20);
  }
  ULONG client{};if(!connected||!::GetNamedPipeClientProcessId(pipe.value,&client)||client!=parent)return 3;
  DWORD mode=PIPE_READMODE_BYTE|PIPE_WAIT;if(!::SetNamedPipeHandleState(pipe.value,&mode,nullptr,nullptr))return 3;
  std::vector<wchar_t> image(32768);const auto count=::GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
  if(!count||count>=image.size())return 3;
  Reply ready;if(!write_packet(pipe.value,&ready,sizeof(ready)))return 3;
  HANDLE child{};
  struct ChildClose { HANDLE& value; ~ChildClose(){if(value)::CloseHandle(value);} } close_child{child};
  for(;;){
    Request request;if(!read_packet(pipe.value,&request,sizeof(request),owner.value,0))return 0;
    if(request.tag!=magic||!nonce_valid(std::wstring_view(request.nonce.data(),request.nonce.size())))return 3;
    Reply reply;
    if(child&&::WaitForSingleObject(child,0)!=WAIT_OBJECT_0){reply.error=ERROR_BUSY;}
    else {
      if(child){::CloseHandle(child);child=nullptr;}
      const std::wstring suffix(request.nonce.data(),request.nonce.size());
      std::wstring command=L"\""+std::wstring(image.data())+L"\" --owner "+std::to_wstring(parent)+L" --pipe "+suffix+L" --nonce "+suffix;
      STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
      PROCESS_INFORMATION process{};
      // The only executable and arguments are fixed here; no shell or page command.
      if(!::CreateProcessW(image.data(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))reply.error=::GetLastError();
      else {
        ::CloseHandle(process.hThread);child=process.hProcess;HANDLE remote{};
        if(!::DuplicateHandle(::GetCurrentProcess(),child,owner.value,&remote,SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)){
          reply.error=::GetLastError();::TerminateProcess(child,3); // Child has not received any operation or secret.
        }else reply.handle=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(remote));
      }
    }
    if(!write_packet(pipe.value,&reply,sizeof(reply)))return 0;
  }
 }catch(...){return 3;}
}
}
