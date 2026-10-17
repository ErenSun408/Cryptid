#include "card/vikey/vikey_helper_contact_store.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "card/diagnostic_error.h"
#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

[[nodiscard]] bool capacity_failure(const std::system_error& error) noexcept {
  return error.code() == make_error_code(CardError::invalid_configuration) &&
         diagnostic_stage(error) ==
             static_cast<std::uint32_t>(DiagnosticStage::sdk_capacity);
}

[[noreturn]] void rethrow_storage_failure() {
  try {
    throw;
  } catch (const contacts::ContactStoreCapacityError&) {
    throw;
  } catch (const std::system_error& error) {
    if (capacity_failure(error)) throw contacts::ContactStoreCapacityError();
    if (error.code().category() == card_error_category()) throw;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("card/vikey/vikey_helper_contact_store", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
  throw std::system_error(
      make_error_code(CardError::backend_unavailable),
      "ViKey credential provider or contact list helper failed");
}

}  // namespace

VikeyHelperContactStoreStorage::VikeyHelperContactStoreStorage(
    VikeyHelperBackendConfig config,
    IVikeyCredentialProvider& credential_provider,
    const std::uint32_t expected_hardware_id)
    : config_(std::move(config)),
      credential_provider_(credential_provider),
      expected_hardware_id_(expected_hardware_id) {
  if (expected_hardware_id_ == 0) {
    throw std::system_error(
        make_error_code(CardError::invalid_configuration),
        "ViKey contact list storage requires a hardware identity");
  }
}

security::SecureBuffer VikeyHelperContactStoreStorage::read(
    const std::size_t offset, const std::size_t length) {
  if (offset != contacts::kContactStoreV1RegionOffset ||
      length != contacts::kContactStoreV1RegionSize) {
    throw std::invalid_argument("contact list reads cover the whole region");
  }
  try {
    VikeyCredentialSet credentials = credential_provider_.load_credentials();
    return read_vikey_development_contact_region_with_network_access(
        config_.helper_path, config_.helper_digest, config_.vendor_directory,
        config_.vendor_digest, expected_hardware_id_, credentials.user_sn,
        credentials.api_key, credentials.user_pin, config_.timeout);
  } catch (...) {
    rethrow_storage_failure();
  }
}

void VikeyHelperContactStoreStorage::write(
    const std::size_t offset, const std::span<const std::byte> bytes) {
  const auto* const at = std::ranges::find(contacts::kContactStoreV1CopyOffsets,
                                           offset);
  if (at == std::ranges::end(contacts::kContactStoreV1CopyOffsets) ||
      bytes.size() != contacts::kContactStoreV1CopySize) {
    throw std::invalid_argument("contact list writes cover one whole copy");
  }
  const std::size_t index = static_cast<std::size_t>(
      at - std::ranges::begin(contacts::kContactStoreV1CopyOffsets));
  try {
    security::SecureBuffer copy(bytes.size());
    std::ranges::copy(bytes, copy.bytes().begin());
    VikeyCredentialSet credentials = credential_provider_.load_credentials();
    write_vikey_development_contact_copy_with_network_access(
        config_.helper_path, config_.helper_digest, config_.vendor_directory,
        config_.vendor_digest, expected_hardware_id_, credentials.user_sn,
        credentials.api_key, credentials.user_pin, index, copy,
        config_.write_timeout);
  } catch (...) {
    rethrow_storage_failure();
  }
}

}  // namespace usb_vault::card::vikey
