#pragma once

#include "card/diagnostic_error.h"

namespace usb_vault::card {
// Stable local step numbers are documented in startup-failure-codes.md.
// These values never contain token contents, SIDs, paths or credentials.
inline constexpr std::uint32_t kProcessSecurityStepCount = 89;

[[noreturn]] inline void throw_process_security(const std::uint32_t step,
    const char* context, const std::uint32_t native = 0,
    const NativeCodeKind kind = NativeCodeKind::none) {
  throw DiagnosticError(make_error_code(CardError::backend_unavailable),
      DiagnosticStage::process_security, context, native, kind, step);
}
}  // namespace usb_vault::card
