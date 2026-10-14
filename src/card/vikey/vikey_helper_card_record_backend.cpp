#include "card/vikey/vikey_helper_card_record_backend.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/card/card_error.h"

#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

[[noreturn]] void rethrow_backend_failure() {
  try {
    throw;
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) throw;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_record_backend", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
  throw std::system_error(
      make_error_code(CardError::backend_unavailable),
      "ViKey credential provider or card-record helper backend failed");
}

}  // namespace

VikeyHelperCardRecordBackend::VikeyHelperCardRecordBackend(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider,
    const std::uint32_t expected_hardware_id)
    : config_(std::move(config)),
      credential_provider_(credential_provider),
      expected_hardware_id_(expected_hardware_id) {
  if (expected_hardware_id_ == 0) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey card-record backend requires a hardware identity");
  }
}

security::SecureBuffer VikeyHelperCardRecordBackend::read_record() {
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    return read_vikey_development_card_record_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        expected_hardware_id_,
        credentials.user_sn, credentials.api_key, credentials.user_pin,
        config_.timeout);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_record_backend", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    rethrow_backend_failure();
  }
}

void VikeyHelperCardRecordBackend::write_record(
    const security::SecureBuffer& record) {
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    write_vikey_development_card_record_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        expected_hardware_id_,
        credentials.user_sn, credentials.api_key, credentials.user_pin,
        record, config_.write_timeout);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_card_record_backend", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    rethrow_backend_failure();
  }
}

}  // namespace usb_vault::card::vikey
