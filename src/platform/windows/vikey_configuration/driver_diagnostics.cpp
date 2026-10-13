#include "driver_diagnostics.h"
#include "card/diagnostic_error.h"
#include <objbase.h>
#include <sddl.h>
#include <array>
#include <cstdio>
#include <vector>

namespace configuration::driver {
namespace {
constexpr std::uint32_t kMagic = 0x31445655; // UVD1
using Packet = std::array<std::uint32_t, 6>;
std::wstring pipe_name(std::wstring_view token) {
  return L"\\\\.\\pipe\\USBVaultDriverDiagnostic-" + std::wstring(token);
}
[[noreturn]] void fail(DWORD code) {
  throw std::system_error(static_cast<int>(code), std::system_category(), "driver diagnostic transport");
}
struct LocalMemory {
  void* value{};
  ~LocalMemory() { if (value) LocalFree(value); }
};
struct Handle {
  HANDLE value{};
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}
void capture_exception(Diagnostic& diagnostic, const std::system_error& error) noexcept {
  if (const auto* value = dynamic_cast<const usb_vault::card::DiagnosticError*>(&error)) {
    diagnostic.detail = static_cast<std::uint32_t>(value->stage());
    diagnostic.code = value->native();
    diagnostic.kind = static_cast<CodeKind>(value->kind());
  } else if (error.code().category() == std::system_category()) {
    diagnostic.code = static_cast<std::uint32_t>(error.code().value());
    diagnostic.kind = CodeKind::win32;
  }
}
std::string format_diagnostic(const Diagnostic& diagnostic) {
  std::array<char, 96> text{};
  std::snprintf(text.data(), text.size(), "UVD1|%u|%u|0x%08X|%u",
      static_cast<unsigned>(diagnostic.step), static_cast<unsigned>(diagnostic.kind),
      static_cast<unsigned>(diagnostic.code), static_cast<unsigned>(diagnostic.detail));
  return text.data();
}
bool valid_diagnostic_token(std::wstring_view token) noexcept {
  if (token.size() != 32) return false;
  for (const auto c : token)
    if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
  return true;
}
DiagnosticReceiver::DiagnosticReceiver() {
  GUID id{};
  const auto status = CoCreateGuid(&id);
  if (FAILED(status))
    throw usb_vault::card::DiagnosticError(usb_vault::card::make_error_code(usb_vault::card::CardError::backend_unavailable),
        usb_vault::card::DiagnosticStage::none, "create driver diagnostic token", static_cast<std::uint32_t>(status),
        usb_vault::card::NativeCodeKind::hresult);
  std::array<wchar_t, 33> token{};
  swprintf_s(token.data(), token.size(), L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
      static_cast<unsigned>(id.Data1), static_cast<unsigned>(id.Data2), static_cast<unsigned>(id.Data3),
      id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
  token_ = token.data();
  Handle current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &current_token.value)) fail(GetLastError());
  DWORD size{};
  GetTokenInformation(current_token.value, TokenUser, nullptr, 0, &size);
  if (!size) fail(GetLastError());
  std::vector<unsigned char> user(size);
  if (!GetTokenInformation(current_token.value, TokenUser, user.data(), size, &size)) fail(GetLastError());
  LPWSTR sid{};
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) fail(GetLastError());
  LocalMemory sid_guard{sid};
  const std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")";
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) fail(GetLastError());
  LocalMemory descriptor_guard{descriptor};
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  const auto name = pipe_name(token_);
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!event_) fail(GetLastError());
  pipe_ = CreateNamedPipeW(name.c_str(),
      PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, &security);
  if (pipe_ == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError(); CloseHandle(event_); event_ = nullptr; fail(error);
  }
  connection_.hEvent = event_;
  if (!ConnectNamedPipe(pipe_, &connection_)) {
    const auto error = GetLastError();
    if (error == ERROR_IO_PENDING) connecting_ = true;
    else if (error != ERROR_PIPE_CONNECTED) {
      CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE;
      CloseHandle(event_); event_ = nullptr; fail(error);
    }
  }
}
DiagnosticReceiver::~DiagnosticReceiver() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    if (connecting_) {
      CancelIoEx(pipe_, &connection_);
      DWORD ignored{};
      GetOverlappedResult(pipe_, &connection_, &ignored, TRUE);
    }
    CloseHandle(pipe_);
  }
  if (event_) CloseHandle(event_);
}
bool DiagnosticReceiver::receive(DWORD child_pid, DWORD exit_code, Diagnostic& diagnostic) noexcept {
  if (!child_pid) return false;
  if (connecting_) {
    if (WaitForSingleObject(event_, 1000) != WAIT_OBJECT_0) return false;
    DWORD ignored{};
    const bool connected = GetOverlappedResult(pipe_, &connection_, &ignored, FALSE) != FALSE;
    connecting_ = false;
    if (!connected) return false;
  }
  ULONG peer{};
  if (!GetNamedPipeClientProcessId(pipe_, &peer) || peer != child_pid) return false;
  Packet packet{};
  OVERLAPPED read{};
  Handle read_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  if (!read_event.value) return false;
  read.hEvent = read_event.value;
  DWORD count{};
  if (!ReadFile(pipe_, packet.data(), sizeof(packet), &count, &read)) {
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(read.hEvent, 1000) != WAIT_OBJECT_0) {
      CancelIoEx(pipe_, &read);
      GetOverlappedResult(pipe_, &read, &count, TRUE);
      return false;
    }
    if (!GetOverlappedResult(pipe_, &read, &count, FALSE)) return false;
  }
  if (count != sizeof(packet) || packet[0] != kMagic || packet[1] != exit_code ||
      packet[2] > static_cast<std::uint32_t>(Step::diagnostic_transport) ||
      packet[3] > static_cast<std::uint32_t>(CodeKind::process_exit)) return false;
  diagnostic = {static_cast<Step>(packet[2]), static_cast<CodeKind>(packet[3]), packet[4], packet[5]};
  return true;
}
bool send_diagnostic(std::wstring_view token, setup::Result result, const Diagnostic& diagnostic) noexcept {
  if (!valid_diagnostic_token(token)) return false;
  try {
    Handle pipe{CreateFileW(pipe_name(token).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (pipe.value == INVALID_HANDLE_VALUE) return false;
    const Packet packet{kMagic, static_cast<std::uint32_t>(result), static_cast<std::uint32_t>(diagnostic.step),
        static_cast<std::uint32_t>(diagnostic.kind), diagnostic.code, diagnostic.detail};
    DWORD count{};
    return WriteFile(pipe.value, packet.data(), sizeof(packet), &count, nullptr) && count == sizeof(packet);
  } catch (...) { return false; }
}
}
