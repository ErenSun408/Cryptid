#include "card/vikey/vikey_session.h"

#include "card/vikey/vikey_module.h"
#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <cstddef>
#include <system_error>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void fail(const CardError error, const char* const context) {
  throw std::system_error(make_error_code(error), context);
}

void require_success(const sdk::Status status, const char* const context) {
  const std::error_code error = map_status(status);
  if (error) throw std::system_error(error, context);
}

[[nodiscard]] bool ascii_hex(const std::byte value) noexcept {
  const unsigned int byte = std::to_integer<unsigned int>(value);
  return (byte >= static_cast<unsigned int>('0') &&
          byte <= static_cast<unsigned int>('9')) ||
         (byte >= static_cast<unsigned int>('A') &&
          byte <= static_cast<unsigned int>('F')) ||
         (byte >= static_cast<unsigned int>('a') &&
          byte <= static_cast<unsigned int>('f'));
}

[[nodiscard]] bool ascii_alphanumeric(const std::byte value) noexcept {
  const unsigned int byte = std::to_integer<unsigned int>(value);
  return (byte >= static_cast<unsigned int>('0') &&
          byte <= static_cast<unsigned int>('9')) ||
         (byte >= static_cast<unsigned int>('A') &&
          byte <= static_cast<unsigned int>('Z')) ||
         (byte >= static_cast<unsigned int>('a') &&
          byte <= static_cast<unsigned int>('z'));
}

void validate_credentials(const security::SecureBuffer& user_sn,
                          const security::SecureBuffer& api_key,
                          const security::SecureBuffer& user_pin) {
  if (user_sn.size() != kVikeyUserSnBytes ||
      !std::ranges::all_of(user_sn.bytes(), ascii_hex)) {
    fail(CardError::invalid_configuration,
         "ViKey UserSN must contain exactly 32 hexadecimal bytes");
  }
  if (api_key.size() != kVikeyApiKeyBytes ||
      !std::ranges::all_of(api_key.bytes(), ascii_hex)) {
    fail(CardError::invalid_configuration,
         "ViKey API key must contain exactly 128 hexadecimal bytes");
  }
  if (user_pin.size() != kVikeyUserPinBytes ||
      !std::ranges::all_of(user_pin.bytes(), ascii_alphanumeric)) {
    fail(CardError::invalid_configuration,
         "ViKey user PIN must contain exactly 8 alphanumeric bytes");
  }
}

void validate_functions(const sdk::Functions& functions) {
  if (functions.set_user_sn == nullptr || functions.find_ex == nullptr ||
      functions.set_api_key == nullptr ||
      functions.uninitialization == nullptr ||
      functions.get_level == nullptr ||
      functions.get_user_data_size == nullptr ||
      functions.get_user_password_attempt == nullptr ||
      functions.user_login == nullptr || functions.logoff == nullptr ||
      functions.read_data == nullptr || functions.write_data == nullptr) {
    fail(CardError::invalid_configuration,
         "ViKey session function table is incomplete");
  }
}

void require_active(const bool active) {
  if (!active) {
    fail(CardError::invalid_operation,
         "ViKey user data operation requires an active session");
  }
}

void require_blob_capacity(const sdk::Functions& functions,
                           const sdk::Index index) {
  sdk::Length bytes = 0;
  require_success(functions.get_user_data_size(index, &bytes),
                  "read ViKey user data capacity");
  if (bytes < kVikeyBlobBytes) {
    fail(CardError::invalid_configuration,
         "ViKey user data area is too small for the fixed blob");
  }
}

[[nodiscard]] security::SecureBuffer terminated_copy(
    const security::SecureBuffer& source) {
  security::SecureBuffer copy(source.size() + 1);
  std::ranges::copy(source.bytes(), copy.bytes().begin());
  copy.bytes().back() = std::byte{};
  return copy;
}

template <typename Operation>
[[nodiscard]] sdk::Status call_with_secret(
    const security::SecureBuffer& secret, Operation&& operation) {
  security::SecureBuffer argument = terminated_copy(secret);
  return operation(reinterpret_cast<char*>(argument.data()));
}

[[nodiscard]] std::error_code cleanup_session(
    const sdk::Functions& functions, const sdk::Index index,
    const bool logged_in, const bool initialized) noexcept {
  std::error_code first_error;
  if (logged_in) first_error = map_status(functions.logoff(index));
  if (initialized) {
    const std::error_code uninitialization_error =
        map_status(functions.uninitialization());
    if (!first_error) first_error = uninitialization_error;
  }
  return first_error;
}

class PendingSession final {
 public:
  explicit PendingSession(const sdk::Functions& functions) noexcept
      : functions_(functions) {}

  ~PendingSession() noexcept {
    if (owned_) {
      static_cast<void>(cleanup_session(functions_, index_, logged_in_, true));
    }
  }

  PendingSession(const PendingSession&) = delete;
  PendingSession& operator=(const PendingSession&) = delete;

  void arm() noexcept { owned_ = true; }

  void mark_logged_in(const sdk::Index index) noexcept {
    index_ = index;
    logged_in_ = true;
  }

  void release() noexcept { owned_ = false; }

 private:
  sdk::Functions functions_{};
  sdk::Index index_{};
  bool logged_in_{};
  bool owned_{};
};

}  // namespace

VikeySession::VikeySession(sdk::Functions functions,
                           const sdk::Index index,
                           const sdk::Byte attempts_remaining,
                           const sdk::Byte attempts_maximum) noexcept
    : functions_(functions),
      index_(index),
      attempts_remaining_(attempts_remaining),
      attempts_maximum_(attempts_maximum) {}

VikeySession::~VikeySession() noexcept { static_cast<void>(close()); }

VikeySession VikeySession::open(
    const sdk::Functions& functions,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin) {
  validate_credentials(user_sn, api_key, user_pin);
  validate_functions(functions);
  PendingSession pending(functions);

  {
    security::SecureBuffer user_sn_argument = terminated_copy(user_sn);
    pending.arm();
    require_success(functions.set_user_sn(
                        reinterpret_cast<char*>(user_sn_argument.data())),
                    "configure ViKey UserSN");
  }

  std::uint32_t count = 0;
  require_success(functions.find_ex(&count), "find ViKey device");
  if (count == 0) {
    fail(CardError::device_not_found, "find one ViKey device");
  }
  if (count != 1) {
    fail(CardError::invalid_operation,
         "ViKey session requires exactly one connected device");
  }
  constexpr sdk::Index kSelectedIndex = 0;

  require_success(
      call_with_secret(api_key, [&](char* const value) {
        return functions.set_api_key(kSelectedIndex, value);
      }),
      "configure ViKey API key");

  sdk::Byte current_attempt = 0;
  sdk::Byte maximum_attempt = 0;
  require_success(functions.get_user_password_attempt(
                      kSelectedIndex, &current_attempt, &maximum_attempt),
                  "read ViKey user PIN attempts");
  if (maximum_attempt != 0 && current_attempt > maximum_attempt) {
    fail(CardError::device_failure,
         "ViKey reported inconsistent user PIN attempts");
  }
  if (maximum_attempt != 0 && current_attempt == 0) {
    fail(CardError::authentication_locked,
         "ViKey user authentication is locked");
  }

  require_success(
      call_with_secret(user_pin, [&](char* const value) {
        return functions.user_login(kSelectedIndex, value);
      }),
      "authenticate ViKey user PIN");
  pending.mark_logged_in(kSelectedIndex);

  sdk::Byte level = 0;
  require_success(functions.get_level(kSelectedIndex, &level),
                  "verify ViKey user permission level");
  constexpr sdk::Byte kUserLevel = 1;
  if (level != kUserLevel) {
    fail(CardError::device_failure,
         "ViKey login succeeded without user permission level");
  }

  pending.release();
  return VikeySession(functions, kSelectedIndex, current_attempt,
                      maximum_attempt);
}

std::error_code VikeySession::close() noexcept {
  const bool logged_in = logged_in_;
  const bool initialized = initialized_;
  logged_in_ = false;
  initialized_ = false;
  return cleanup_session(functions_, index_, logged_in, initialized);
}

bool VikeySession::active() const noexcept {
  return logged_in_ && initialized_;
}

sdk::Index VikeySession::index() const noexcept { return index_; }

sdk::Byte VikeySession::user_pin_attempts_remaining() const noexcept {
  return attempts_remaining_;
}

sdk::Byte VikeySession::user_pin_attempts_maximum() const noexcept {
  return attempts_maximum_;
}

security::SecureBuffer VikeySession::read_user_blob() {
  require_active(active());
  require_blob_capacity(functions_, index_);
  security::SecureBuffer result(kVikeyBlobBytes);
  require_success(
      functions_.read_data(
          index_, kVikeyBlobAddress,
          static_cast<sdk::Length>(result.size()),
          reinterpret_cast<sdk::Byte*>(result.data())),
      "read ViKey user blob");
  return result;
}

void VikeySession::write_user_blob(
    const security::SecureBuffer& blob) {
  if (blob.size() != kVikeyBlobBytes) {
    fail(CardError::invalid_configuration,
         "ViKey user blob must contain exactly 32 bytes");
  }
  require_active(active());
  require_blob_capacity(functions_, index_);
  security::SecureBuffer argument(blob.size());
  std::ranges::copy(blob.bytes(), argument.bytes().begin());
  require_success(
      functions_.write_data(
          index_, kVikeyBlobAddress,
          static_cast<sdk::Length>(argument.size()),
          reinterpret_cast<sdk::Byte*>(argument.data())),
      "write ViKey user blob");
}

}  // namespace usb_vault::card::vikey
