#pragma once

#include "card/diagnostic_error.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "card/process_security_failure.h"
#include "usb_vault/card/failure_diagnostic.h"

#include <exception>
#include <new>
#include <utility>

namespace usb_vault::card {

// Local startup failures need no allocated exception message or secret data.
class LocalFailureError final : public std::exception {
 public:
  explicit LocalFailureError(FailureDiagnostic failure) noexcept : failure_(failure) {}
  [[nodiscard]] FailureDiagnostic failure() const noexcept { return failure_; }
  const char* what() const noexcept override { return "local startup failure"; }
 private:
  FailureDiagnostic failure_;
};

// Retains only the numeric result, never the original exception or its text.
class CapturedFailureError final : public std::system_error {
 public:
  explicit CapturedFailureError(FailureDiagnostic failure)
      : std::system_error(make_error_code(
            failure.primary.reason >= 1 && failure.primary.reason <= 18
                ? static_cast<CardError>(failure.primary.reason)
                : CardError::backend_unavailable),
                          "captured card operation failure"), failure_(failure) {}
  [[nodiscard]] FailureDiagnostic failure() const noexcept { return failure_; }
 private:
  FailureDiagnostic failure_;
};

[[nodiscard]] inline FailureDiagnostic capture_failure(
    const std::exception_ptr& failure) noexcept {
  if (!failure) return {};
  try {
    std::rethrow_exception(failure);
  } catch (const LocalFailureError& error) {
    USB_VAULT_TRACE_FAILURE("card/capture_failure", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return error.failure();
  } catch (const CapturedFailureError& error) {
    USB_VAULT_TRACE_FAILURE("card/capture_failure", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return error.failure();
  } catch (const std::system_error& error) {
    USB_VAULT_TRACE_FAILURE("card/capture_failure", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    FailureDetail detail{.code = 9001};
    if (error.code().category() == card_error_category() &&
        error.code().value() >= 1 && error.code().value() <= 18) {
      detail.reason = static_cast<std::uint32_t>(error.code().value());
      detail.code = 3000 + detail.reason;
    }
    if (const auto* diagnostic = dynamic_cast<const DiagnosticError*>(&error)) {
      const auto stage = static_cast<std::uint32_t>(diagnostic->stage());
      if (stage > 0 && stage <= 38) detail.code = 2000 + stage;
      else if (stage > 38) detail.code = 9001;
      if (stage == 11 && diagnostic->step() <= kProcessSecurityStepCount)
        detail.step = diagnostic->step();
      const auto kind = static_cast<std::uint32_t>(diagnostic->kind());
      if (kind >= 1 && kind <= 3) {
        detail.native_kind = kind;
        detail.native_code = diagnostic->native();
      }
    } else if (error.code().category() == std::system_category()) {
      detail.native_kind = static_cast<std::uint32_t>(NativeCodeKind::win32);
      detail.native_code = static_cast<std::uint32_t>(error.code().value());
    }
    return {.primary = detail};
  } catch (const std::bad_alloc&) {
    USB_VAULT_TRACE_FAILURE("card/capture_failure", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return {.primary = {.code = 9002}};
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/capture_failure", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return {.primary = {.code = 9001}};
  }
}

[[nodiscard]] inline FailureDiagnostic capture_startup_failure(
    const std::uint32_t fallback, const std::exception_ptr& error = std::current_exception()) noexcept {
  auto failure = capture_failure(error);
  if (failure.primary.code == 0 || failure.primary.code == 9001)
    failure.primary.code = fallback;
  return failure;
}

[[noreturn]] inline void throw_startup_failure(const std::uint32_t code,
    const std::uint32_t native_kind = 0, const std::uint32_t native_code = 0) {
  throw LocalFailureError({.primary = {.code = code, .native_kind = native_kind,
                                     .native_code = native_code}});
}

// Always run cleanup exactly once. A secondary error must never replace the
// operation's primary diagnostic. Bounded storage keeps the first secondary.
template <typename Cleanup>
void cleanup_preserving_failure(const std::exception_ptr& primary,
                                Cleanup&& cleanup) {
  try {
    std::forward<Cleanup>(cleanup)();
  } catch (...) {
    if (!primary) throw;
    auto failure = capture_failure(primary);
    if (failure.cleanup.code == 0)
      failure.cleanup = capture_failure(std::current_exception()).primary;
    throw CapturedFailureError(failure);
  }
  if (primary) std::rethrow_exception(primary);
}

}  // namespace usb_vault::card
