#include "card/vikey/vikey_card_journal_store.h"

#include "card/vikey/card_journal_block.h"
#include "card/card_record_journal_v1_internal.h"
#include "card/vikey/card_operation_lease.h"
#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {
[[noreturn]] void fail(const CardError error, const char* context) {
  throw std::system_error(make_error_code(error), context);
}
security::SecureBuffer copy(std::span<const std::byte> bytes) {
  security::SecureBuffer result(bytes.size());
  std::ranges::copy(bytes, result.bytes().begin());
  return result;
}
VikeyCredentialSet load_credentials(IVikeyCredentialProvider& provider) {
  try { return provider.load_credentials(); }
  catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) throw;
  } catch (...) {}
  fail(CardError::backend_unavailable, "journal credential provider failed");
}

class TransactionStorage final : public ICardRecordJournalStorage {
 public:
  explicit TransactionStorage(IVikeyCardJournalTransport& transport)
      : transport_(transport), snapshot_(fetch()) {}
  std::size_t capacity() override { return kCardRecordJournalV2StorageSize; }
  security::SecureBuffer read(std::size_t offset, std::size_t length) override {
    require_live();
    if (offset > snapshot_.size() || length > snapshot_.size() - offset) {
      fail(CardError::invalid_configuration, "journal read outside reserved snapshot");
    }
    return copy(snapshot_.bytes().subspan(offset, length));
  }
  void write(std::size_t offset, std::span<const std::byte> bytes) override {
    require_live();
    if (!valid_card_journal_block(offset, bytes.size())) {
      fail(CardError::invalid_configuration, "journal write outside fixed blocks");
    }
    auto block = copy(bytes);
    auto intended = copy(snapshot_.bytes());
    std::ranges::copy(bytes, intended.bytes().begin() + offset);
    std::exception_ptr reported;
    try { transport_.write_block(snapshot_, offset, block); }
    catch (...) { reported = std::current_exception(); }
    try {
      auto observed = fetch();
      if (!std::ranges::equal(observed.bytes(), intended.bytes())) {
        if (reported) std::rethrow_exception(reported);
        fail(CardError::device_failure, "journal complete postimage mismatch");
      }
      snapshot_ = std::move(observed);
    } catch (...) {
      // The journal core catches write errors and reads the target back. Keep
      // all later reads closed unless the complete postimage was verified:
      // matching only the target must not hide corruption elsewhere.
      failure_ = std::current_exception();
      throw;
    }
    if (reported) std::rethrow_exception(reported);
  }
  void verify_current() {
    require_live();
    const auto observed = fetch();
    if (!std::ranges::equal(observed.bytes(), snapshot_.bytes())) {
      fail(CardError::device_failure, "journal changed before transaction completion");
    }
  }
 private:
  security::SecureBuffer fetch() {
    auto result = transport_.snapshot_reserved();
    if (result.size() != kCardRecordJournalV2StorageSize) {
      fail(CardError::device_failure, "incomplete reserved journal snapshot");
    }
    return result;
  }
  void require_live() const { if (failure_) std::rethrow_exception(failure_); }
  IVikeyCardJournalTransport& transport_;
  security::SecureBuffer snapshot_;
  std::exception_ptr failure_;
};
} // namespace

VikeyHelperCardJournalTransport::VikeyHelperCardJournalTransport(
    VikeyHelperBackendConfig config, IVikeyCredentialProvider& credentials,
    const std::uint32_t expected_hardware_id)
    : config_(std::move(config)), credentials_(credentials), hardware_id_(expected_hardware_id) {
  if (hardware_id_ == 0) fail(CardError::invalid_configuration, "journal transport requires hardware identity");
}
security::SecureBuffer VikeyHelperCardJournalTransport::snapshot_reserved() {
  auto credentials = load_credentials(credentials_);
  return snapshot_vikey_development_card_journal_with_network_access(config_.helper_path,
      config_.helper_digest, config_.vendor_directory, config_.vendor_digest, hardware_id_,
      credentials.user_sn, credentials.api_key, credentials.user_pin, config_.timeout);
}
void VikeyHelperCardJournalTransport::write_block(const security::SecureBuffer& expected,
    const std::size_t offset, const security::SecureBuffer& replacement) {
  if (expected.size() != kCardRecordJournalV2StorageSize || !valid_card_journal_block(offset, replacement.size())) {
    fail(CardError::invalid_configuration, "invalid helper journal block");
  }
  auto credentials = load_credentials(credentials_);
  write_vikey_development_card_journal_block_with_network_access(config_.helper_path,
      config_.helper_digest, config_.vendor_directory, config_.vendor_digest, hardware_id_,
      credentials.user_sn, credentials.api_key, credentials.user_pin, expected,
      static_cast<std::uint32_t>(offset), replacement, config_.timeout);
}
security::SecureBuffer VikeyCardJournalStore::snapshot() {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto record = snapshot_card_record_journal_v2(storage);
  storage.verify_current();
  return record;
}
void VikeyCardJournalStore::recover(const std::span<const std::byte> expected,
    const security::SecureBuffer& root) {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  recover_card_record_journal_v2(storage, expected, root);
  storage.verify_current();
}
void VikeyCardJournalStore::commit(const std::span<const std::byte> expected,
    const std::span<const std::byte> replacement, const security::SecureBuffer& root) {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  commit_card_record_journal_v2(storage, expected, replacement, root);
  storage.verify_current();
}
security::SecureBuffer VikeyCardJournalStore::snapshot_legacy() {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto selected = detail::snapshot_legacy_card_journal(storage);
  if (inspect_card_record(selected.record.bytes()) != CardRecordState::initialized_v1) {
    fail(CardError::invalid_operation, "upgrade requires initialized legacy record");
  }
  storage.verify_current();
  return std::move(selected.record);
}

PreparedVikeyCardUpgrade VikeyCardJournalStore::prepare_upgrade(
    const std::span<const std::byte> expected_v1, security::SecureBuffer password) {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto plan = prepare_card_journal_v2_migration(storage, expected_v1, std::move(password));
  storage.verify_current();
  return PreparedVikeyCardUpgrade(std::move(plan), owner_);
}

void VikeyCardJournalStore::commit_upgrade(PreparedVikeyCardUpgrade&& prepared) {
  // Consume even a rejected attempt; never leave a stale confirmation object
  // available for implicit retry or use by a different card/store instance.
  auto attempt = std::move(prepared);
  if (attempt.owner_ != owner_) {
    fail(CardError::invalid_operation, "upgrade preparation belongs to another store or was consumed");
  }
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  commit_card_journal_v2_migration(storage, attempt.plan_);
  storage.verify_current();
}

security::SecureBuffer VikeyCardJournalStore::snapshot_provisioning() {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto selected = detail::snapshot_legacy_card_journal(storage);
  if (inspect_card_record(selected.record.bytes()) != CardRecordState::provisioned_uninitialized) {
    fail(CardError::invalid_operation, "initialization requires provisioning marker");
  }
  storage.verify_current();
  return std::move(selected.record);
}

PreparedVikeyCardInitialization VikeyCardJournalStore::prepare_initialization(
    const std::span<const std::byte> expected, security::ConfirmedPassword password) {
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto plan = prepare_card_journal_v2_initialization(storage, expected, std::move(password).take());
  storage.verify_current();
  return PreparedVikeyCardInitialization(std::move(plan), owner_);
}

security::SecureBuffer VikeyCardJournalStore::initialize(
    const PreparedVikeyCardInitialization& prepared) {
  if (prepared.owner_ != owner_) {
    fail(CardError::invalid_operation, "initialization preparation belongs to another store or was moved");
  }
  CardOperationLease lease;
  TransactionStorage storage(transport_);
  auto root = commit_card_journal_v2_initialization(storage, prepared.plan_);
  storage.verify_current();
  return root;
}

} // namespace usb_vault::card::vikey
