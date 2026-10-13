#pragma once
#include "setup_policy.h"
#include <Windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace configuration::driver {
// Append-only step IDs. Never carry exception text, paths or secrets. Keep the
// display mapping in packaging/windows/driver-diagnostics.nsh synchronized.
enum class Step : std::uint32_t {
  none, helper_initialization, system_path, service_manager, service_open,
  driver_presence, device_open, service_configuration, existing_verification,
  service_status, driver_version, elevation_token, source_verification,
  target_directory, target_security, target_create, source_open, copy_read,
  copy_write, copy_flush, target_verification, service_create, service_start,
  post_install_probe, launch_elevated, wait_elevated, diagnostic_transport
};
enum class CodeKind : std::uint32_t { none, win32, hresult, process_exit };
struct Diagnostic {
  Step step{};
  CodeKind kind{};
  std::uint32_t code{};
  std::uint32_t detail{};
};
void capture_exception(Diagnostic& diagnostic, const std::system_error& error) noexcept;
std::string format_diagnostic(const Diagnostic& diagnostic);
bool valid_diagnostic_token(std::wstring_view token) noexcept;
bool send_diagnostic(std::wstring_view token, setup::Result result,
                     const Diagnostic& diagnostic) noexcept;

// The child PID and its exit code authenticate/correlate this report. The
// diagnostic is never used to authorize installation or decide success.
class DiagnosticReceiver final {
 public:
  DiagnosticReceiver();
  ~DiagnosticReceiver();
  DiagnosticReceiver(const DiagnosticReceiver&) = delete;
  DiagnosticReceiver& operator=(const DiagnosticReceiver&) = delete;
  const std::wstring& token() const noexcept { return token_; }
  bool receive(DWORD child_pid, DWORD exit_code, Diagnostic& diagnostic) noexcept;
 private:
  std::wstring token_;
  HANDLE pipe_{INVALID_HANDLE_VALUE};
  HANDLE event_{};
  OVERLAPPED connection_{};
  bool connecting_{};
};
}
