#pragma once

#include "card/diagnostic_error.h"
#include "usb_vault/card/failure_diagnostic.h"

#include <exception>
#include <new>
#include <utility>

namespace usb_vault::card {

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
  } catch (const CapturedFailureError& error) {
    return error.failure();
  } catch (const std::system_error& error) {
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
    return {.primary = {.code = 9002}};
  } catch (...) {
    return {.primary = {.code = 9001}};
  }
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
