#include "card/vikey/vikey_helper_blob_backend.h"

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
  }
  throw std::system_error(
      make_error_code(CardError::backend_unavailable),
      "ViKey credential provider or helper backend failed");
}

}  // namespace

VikeyHelperBlobBackend::VikeyHelperBlobBackend(
    VikeyHelperBlobBackendConfig config,
    IVikeyCredentialProvider& credential_provider)
    : config_(std::move(config)),
      credential_provider_(credential_provider) {}

security::SecureBuffer VikeyHelperBlobBackend::get_blob() {
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    return read_vikey_development_blob_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        credentials.user_sn, credentials.api_key, credentials.user_pin,
        config_.timeout);
  } catch (...) {
    rethrow_backend_failure();
  }
}

void VikeyHelperBlobBackend::put_blob(
    const security::SecureBuffer& blob) {
  try {
    VikeyCredentialSet credentials =
        credential_provider_.load_credentials();
    write_vikey_development_blob_with_network_access(
        config_.helper_path, config_.helper_digest,
        config_.vendor_directory, config_.vendor_digest,
        credentials.user_sn, credentials.api_key, credentials.user_pin,
        blob, config_.timeout);
  } catch (...) {
    rethrow_backend_failure();
  }
}

}  // namespace usb_vault::card::vikey
