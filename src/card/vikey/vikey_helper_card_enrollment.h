#pragma once

#include "card/vikey/vikey_card_insertion_context.h"
#include "card/vikey/vikey_helper_backend.h"
#include "card/vikey/vikey_helper_card_record_backend.h"
#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/card/card_initialization_service.h"

#include <memory>

namespace usb_vault::card::vikey {

// ViKey-private enrollment boundary for the network-enabled development
// helper. It stores only non-secret artifact configuration and borrows the
// credential provider; every probe/bootstrap operation obtains a fresh
// locked credential set and destroys it after the one-shot helper exits.
class VikeyHelperCardEnrollment final
    : public application::ICardEntryInspectionSource {
 public:
  VikeyHelperCardEnrollment(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider);

  [[nodiscard]] std::unique_ptr<VikeyCardInsertionContext> inspect_entry();
  [[nodiscard]] std::unique_ptr<application::IReadOnlyCardEntryInspection>
  inspect_inserted_card() override;
  [[nodiscard]] std::unique_ptr<ICardInitializationTarget>
  make_initialization_target(
      const application::IReadOnlyCardEntryInspection& context) override;
  [[nodiscard]] std::unique_ptr<ICardLoginTarget> make_login_target(
      const application::IReadOnlyCardEntryInspection& context) override;
  void bootstrap(const VikeyCardMediaProbe& context);

 private:
  [[nodiscard]] VikeyCardMediaProbe probe();

  VikeyHelperBackendConfig config_;
  IVikeyCredentialProvider& credential_provider_;
};

// Binds one adapter-approved initializable insertion context to the formal
// 216-byte ICardStore. Bare media first uses the v9 bootstrap operation; an
// exact provisioning marker observed by the same atomic probe starts already
// verified and skips bootstrap. Only verified state is cached in volatile
// process memory, so a failed Card Record write can be retried without
// rewriting the marker. Every store operation still re-authenticates and
// checks the original hardware identity in the helper.
class VikeyHelperCardInitializationTarget final
    : public ICardInitializationTarget {
 public:
  VikeyHelperCardInitializationTarget(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider,
      VikeyCardMediaProbe context);

  void ensure_provisioning_record() override;
  [[nodiscard]] ICardStore& card_store() noexcept override;

 private:
  enum class ProvisioningState {
    pending,
    verified,
    failed,
  };

  VikeyCardMediaProbe context_;
  VikeyHelperCardEnrollment enrollment_;
  VikeyHelperCardRecordBackend backend_;
  VikeyCardRecordStore store_;
  ProvisioningState provisioning_state_{ProvisioningState::pending};
};

// Binds an adapter-approved initialized insertion to a formal Card Record
// store for one login attempt. Construction is side-effect free; each later
// read obtains fresh vendor credentials and rechecks the captured hardware ID
// through the helper backend.
class VikeyHelperCardLoginTarget final : public ICardLoginTarget {
 public:
  VikeyHelperCardLoginTarget(
      VikeyHelperBackendConfig config,
      IVikeyCredentialProvider& credential_provider,
      VikeyCardMediaProbe context);

  [[nodiscard]] security::SecureBuffer read_record() override;

 private:
  VikeyCardMediaProbe context_;
  VikeyHelperCardRecordBackend backend_;
  VikeyCardRecordStore store_;
};

}  // namespace usb_vault::card::vikey
