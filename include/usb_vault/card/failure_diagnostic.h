#pragma once

#include <cstdint>

namespace usb_vault::card {

// Fixed, volatile diagnostics only. No exception text, paths, card identity,
// credential bytes, or protocol payloads may be stored here. Zero means absent.
struct FailureDetail final {
  std::uint32_t code{};
  std::uint32_t reason{};
  std::uint32_t native_kind{};
  std::uint32_t native_code{};
  std::uint32_t step{};
};

struct FailureDiagnostic final {
  FailureDetail primary{};
  FailureDetail cleanup{};
};

}  // namespace usb_vault::card
