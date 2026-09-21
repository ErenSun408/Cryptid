#pragma once
#include "usb_vault/card/card_error.h"
#include <cstdint>
#include <system_error>
namespace usb_vault::card {
// Numeric, vendor-neutral diagnostic metadata only. No paths or secrets.
enum class DiagnosticStage : std::uint32_t { none = 0,
  credential_sn = 1,
  credential_key = 2,
  credential_pin = 3,
  work_directory = 4,
  path_check = 5,
  file_open = 6,
  file_hash = 7,
  file_state = 8,
  sdk_stage = 9,
  process_start = 10,
  process_security = 11,
  sdk_load = 12,
  sdk_export = 13,
  process_exit = 14,
  cleanup = 15,
  ipc_setup = 16,
  ipc_read = 17,
  ipc_protocol = 18,
  response_timeout = 19,
  data_timeout = 20,
  sdk_identity = 21,
  sdk_find = 22,
  sdk_key = 23,
  sdk_attempts = 24,
  sdk_pin = 25,
  sdk_permission = 26,
  sdk_read = 27,
  sdk_write = 28,
  sdk_capacity = 29,
  sdk_hardware = 30,
  input_shape = 31,
  sdk_calculation1 = 32,
  sdk_calculation2 = 33,
  installer_signature = 34,
  installer_start = 35,
  installer_wait = 36,
  application_start = 37,
  unexpected = 38,
};
enum class NativeCodeKind : std::uint32_t { none, win32, hresult, process_exit };
class DiagnosticError final : public std::system_error {
 public:
  DiagnosticError(std::error_code code, DiagnosticStage stage,
                  const char* context, std::uint32_t native = 0,
                  NativeCodeKind kind = NativeCodeKind::none, std::uint32_t step = 0)
      : std::system_error(code, context), stage_(stage), native_(native), kind_(kind), step_(step) {}
  std::uint32_t step() const noexcept { return step_; }
  DiagnosticStage stage() const noexcept { return stage_; }
  std::uint32_t native() const noexcept { return native_; }
  NativeCodeKind kind() const noexcept { return kind_; }
 private:
  DiagnosticStage stage_;
  std::uint32_t native_;
  NativeCodeKind kind_;
  std::uint32_t step_;
};
inline std::uint32_t diagnostic_stage(const std::system_error& e) noexcept {
  const auto* d = dynamic_cast<const DiagnosticError*>(&e);
  return d ? static_cast<std::uint32_t>(d->stage()) : 0;
}
inline std::uint32_t diagnostic_native(const std::system_error& e) noexcept {
  const auto* d = dynamic_cast<const DiagnosticError*>(&e);
  return d ? d->native() : 0;
}
inline std::uint32_t diagnostic_kind(const std::system_error& e) noexcept {
  const auto* d = dynamic_cast<const DiagnosticError*>(&e);
  return d ? static_cast<std::uint32_t>(d->kind()) : 0;
}
} // namespace usb_vault::card
