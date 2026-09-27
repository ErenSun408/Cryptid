#include "card/vikey/vikey_blob_round_trip.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "usb_vault/card/card_error.h"

#include <cstddef>
#include <exception>
#include <system_error>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void fail(const CardError error, const char* const context) {
  throw std::system_error(make_error_code(error), context);
}

[[nodiscard]] bool equal_blob(
    const security::SecureBuffer& left,
    const security::SecureBuffer& right) noexcept {
  if (left.size() != right.size()) return false;
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left.bytes()[index]) ^
                  std::to_integer<unsigned int>(right.bytes()[index]);
  }
  return difference == 0;
}

}  // namespace

void verify_development_blob_round_trip(
    VikeySession& session, const security::SecureBuffer& test_blob) {
  if (test_blob.size() != kVikeyBlobBytes) {
    fail(CardError::invalid_configuration,
         "development ViKey test blob must contain exactly 32 bytes");
  }

  security::SecureBuffer original = session.read_user_blob();
  std::exception_ptr test_failure;
  try {
    // Restoration is required even when the write reports failure because a
    // vendor or device may have changed only part of the requested range.
    session.write_user_blob(test_blob);
    security::SecureBuffer observed = session.read_user_blob();
    if (!equal_blob(observed, test_blob)) {
      fail(CardError::device_failure,
           "development ViKey test blob did not verify");
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_blob_round_trip", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    test_failure = std::current_exception();
  }

  try {
    session.write_user_blob(original);
    security::SecureBuffer restored = session.read_user_blob();
    if (!equal_blob(restored, original)) {
      fail(CardError::device_failure,
           "development ViKey original blob did not verify after restore");
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_blob_round_trip", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    fail(CardError::device_failure,
         "development ViKey original blob restoration is unverified");
  }

  if (test_failure) std::rethrow_exception(test_failure);
}

}  // namespace usb_vault::card::vikey
