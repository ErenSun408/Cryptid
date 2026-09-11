#pragma once

#include <cstdint>
#include <system_error>
#include <type_traits>

namespace usb_vault::card {

// Stable application-facing failures for replaceable card backends. Vendor
// status values must be mapped to this domain before leaving an adapter.
enum class CardError : std::int32_t {
  backend_unavailable = 1,
  invalid_configuration = 2,
  device_not_found = 3,
  authentication_failed = 4,
  authentication_locked = 5,
  permission_denied = 6,
  unsupported = 7,
  communication_failed = 8,
  invalid_operation = 9,
  device_failure = 10,
  api_key_invalid = 11,
  api_key_missing = 12,
  api_key_calculation_failed = 13,
  api_key_verification_failed = 14,
  user_sn_invalid = 15,
  activation_required = 16,
  multiple_devices = 17,
  operation_timed_out = 18,

};

[[nodiscard]] const std::error_category& card_error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(CardError error) noexcept;

}  // namespace usb_vault::card

template <>
struct std::is_error_code_enum<usb_vault::card::CardError> : true_type {};
