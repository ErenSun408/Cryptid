#include "card/diagnostic_error.h"
#include "card/vikey/vikey_session.h"

#include "card/vikey/vikey_module.h"
#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <system_error>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void fail(const CardError error, const char* const context,
    const DiagnosticStage stage = DiagnosticStage::input_shape) {
  throw DiagnosticError(make_error_code(error), stage, context);
}

void require_success(const sdk::Status status, const char* const context,
                     DiagnosticStage stage = DiagnosticStage::none) {
  const std::error_code error = map_status(status);
  if (status == sdk::kApiKeyCalculate1) stage = DiagnosticStage::sdk_calculation1;
  if (status == sdk::kApiKeyCalculate2) stage = DiagnosticStage::sdk_calculation2;
  if (error) throw DiagnosticError(error, stage, context);
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
    fail(CardError::invalid_configuration, "ViKey UserSN must contain exactly 32 hexadecimal bytes", DiagnosticStage::credential_sn);
  }
  if (api_key.size() != kVikeyApiKeyBytes ||
      !std::ranges::all_of(api_key.bytes(), ascii_hex)) {
    fail(CardError::invalid_configuration, "ViKey API key must contain exactly 128 hexadecimal bytes", DiagnosticStage::credential_key);
  }
  if (user_pin.size() != kVikeyUserPinBytes ||
      !std::ranges::all_of(user_pin.bytes(), ascii_alphanumeric)) {
    fail(CardError::invalid_configuration, "ViKey user PIN must contain exactly 8 alphanumeric bytes", DiagnosticStage::credential_pin);
  }
}

void validate_functions(const sdk::Functions& functions) {
  if (functions.set_user_sn == nullptr || functions.find_ex == nullptr ||
      functions.set_api_key == nullptr ||
      functions.uninitialization == nullptr ||
      functions.get_hid == nullptr ||
      functions.get_level == nullptr ||
      functions.get_user_data_size == nullptr ||
      functions.get_user_password_attempt == nullptr ||
      functions.user_login == nullptr || functions.logoff == nullptr ||
      functions.read_data == nullptr || functions.write_data == nullptr) {
    fail(CardError::invalid_configuration,
         "ViKey session function table is incomplete", DiagnosticStage::sdk_export);
  }
}

[[nodiscard]] std::uint32_t read_hardware_id(
    const sdk::Functions& functions, const sdk::Index index) {
  std::uint32_t hardware_id = 0;
  require_success(functions.get_hid(index, &hardware_id),
                  "read ViKey hardware identity", DiagnosticStage::sdk_hardware);
  if (hardware_id == 0) {
    fail(CardError::device_failure,
         "ViKey reported an unusable hardware identity", DiagnosticStage::sdk_hardware);
  }
  return hardware_id;
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
                  "read ViKey user data capacity", DiagnosticStage::sdk_capacity);
  if (bytes < kVikeyBlobBytes) {
    fail(CardError::invalid_configuration,
         "ViKey user data area is too small for the fixed blob", DiagnosticStage::sdk_capacity);
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
                           const std::uint32_t hardware_id,
                           const sdk::Byte attempts_remaining,
                           const sdk::Byte attempts_maximum) noexcept
    : functions_(functions),
      index_(index),
      hardware_id_(hardware_id),
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
                    "configure ViKey UserSN", DiagnosticStage::sdk_identity);
  }

  std::uint32_t count = 0;
  require_success(functions.find_ex(&count), "find ViKey device", DiagnosticStage::sdk_find);
  if (count == 0) {
    fail(CardError::device_not_found, "find one ViKey device", DiagnosticStage::sdk_find);
  }
  if (count != 1) {
    fail(CardError::multiple_devices,
         "ViKey session requires exactly one connected device", DiagnosticStage::sdk_find);
  }
  constexpr sdk::Index kSelectedIndex = 0;
  const std::uint32_t hardware_id =
      read_hardware_id(functions, kSelectedIndex);

  require_success(
      call_with_secret(api_key, [&](char* const value) {
        return functions.set_api_key(kSelectedIndex, value);
      }),
      "configure ViKey API key", DiagnosticStage::sdk_key);

  sdk::Byte current_attempt = 0;
  sdk::Byte maximum_attempt = 0;
  require_success(functions.get_user_password_attempt(
                      kSelectedIndex, &current_attempt, &maximum_attempt),
                  "read ViKey user PIN attempts", DiagnosticStage::sdk_attempts);
  if (maximum_attempt != 0 && current_attempt > maximum_attempt) {
    fail(CardError::device_failure, "ViKey reported inconsistent user PIN attempts", DiagnosticStage::input_shape);
  }
  if (maximum_attempt != 0 && current_attempt == 0) {
    fail(CardError::authentication_locked,
         "ViKey user authentication is locked", DiagnosticStage::sdk_attempts);
  }

  require_success(
      call_with_secret(user_pin, [&](char* const value) {
        return functions.user_login(kSelectedIndex, value);
      }),
      "authenticate ViKey user PIN", DiagnosticStage::sdk_pin);
  pending.mark_logged_in(kSelectedIndex);

  sdk::Byte level = 0;
  require_success(functions.get_level(kSelectedIndex, &level),
                  "verify ViKey user permission level", DiagnosticStage::sdk_permission);
  constexpr sdk::Byte kUserLevel = 1;
  if (level != kUserLevel) {
    fail(CardError::device_failure,
         "ViKey login succeeded without user permission level", DiagnosticStage::sdk_permission);
  }
  if (read_hardware_id(functions, kSelectedIndex) != hardware_id) {
    fail(CardError::device_failure,
         "ViKey hardware identity changed during authentication", DiagnosticStage::sdk_hardware);
  }

  pending.release();
  return VikeySession(functions, kSelectedIndex, hardware_id, current_attempt,
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

std::uint32_t VikeySession::hardware_id() const noexcept {
  return hardware_id_;
}

sdk::Byte VikeySession::user_pin_attempts_remaining() const noexcept {
  return attempts_remaining_;
}

sdk::Byte VikeySession::user_pin_attempts_maximum() const noexcept {
  return attempts_maximum_;
}

security::SecureBuffer VikeySession::read_user_blob() {
  require_active(active());
  verify_hardware_identity();
  require_blob_capacity(functions_, index_);
  security::SecureBuffer result(kVikeyBlobBytes);
  require_success(
      functions_.read_data(
          index_, kVikeyBlobAddress,
          static_cast<sdk::Length>(result.size()),
          reinterpret_cast<sdk::Byte*>(result.data())),
      "read ViKey user blob", DiagnosticStage::sdk_read);
  return result;
}

void VikeySession::write_user_blob(
    const security::SecureBuffer& blob) {
  if (blob.size() != kVikeyBlobBytes) {
    fail(CardError::invalid_configuration,
         "ViKey user blob must contain exactly 32 bytes");
  }
  require_active(active());
  verify_hardware_identity();
  require_blob_capacity(functions_, index_);
  security::SecureBuffer argument(blob.size());
  std::ranges::copy(blob.bytes(), argument.bytes().begin());
  require_success(
      functions_.write_data(
          index_, kVikeyBlobAddress,
          static_cast<sdk::Length>(argument.size()),
          reinterpret_cast<sdk::Byte*>(argument.data())),
      "write ViKey user blob", DiagnosticStage::sdk_write);
}

std::size_t VikeySession::user_data_capacity() {
  require_active(active());
  verify_hardware_identity();
  sdk::Length bytes = 0;
  require_success(functions_.get_user_data_size(index_, &bytes),
                  "read ViKey card journal capacity", DiagnosticStage::sdk_capacity);
  return bytes;
}

security::SecureBuffer VikeySession::read_user_data(
    const std::size_t offset, const std::size_t length) {
  require_active(active());
  if (offset > std::numeric_limits<sdk::Address>::max() ||
      length > std::numeric_limits<sdk::Length>::max() ||
      offset > std::numeric_limits<std::size_t>::max() - length) {
    fail(CardError::invalid_operation,
         "ViKey card journal read range is invalid");
  }
  verify_hardware_identity();
  security::SecureBuffer result(length);
  require_success(
      functions_.read_data(
          index_, static_cast<sdk::Address>(offset),
          static_cast<sdk::Length>(length),
          reinterpret_cast<sdk::Byte*>(result.data())),
      "read ViKey card journal range", DiagnosticStage::sdk_read);
  return result;
}

void VikeySession::write_user_data(
    const std::size_t offset,
    const std::span<const std::byte> bytes) {
  require_active(active());
  if (offset > std::numeric_limits<sdk::Address>::max() ||
      bytes.size() > std::numeric_limits<sdk::Length>::max() ||
      offset > std::numeric_limits<std::size_t>::max() - bytes.size()) {
    fail(CardError::invalid_operation,
         "ViKey card journal write range is invalid");
  }
  verify_hardware_identity();
  security::SecureBuffer argument(bytes.size());
  std::ranges::copy(bytes, argument.bytes().begin());
  require_success(
      functions_.write_data(
          index_, static_cast<sdk::Address>(offset),
          static_cast<sdk::Length>(argument.size()),
          reinterpret_cast<sdk::Byte*>(argument.data())),
      "write ViKey card journal range", DiagnosticStage::sdk_write);
}

void VikeySession::verify_hardware_identity() const {
  if (read_hardware_id(functions_, index_) != hardware_id_) {
    fail(CardError::device_failure,
         "ViKey hardware identity changed during the active session", DiagnosticStage::sdk_hardware);
  }
}

}  // namespace usb_vault::card::vikey
