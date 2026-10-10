#pragma once

#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/application/card_entry_router.h"

#include <memory>

namespace usb_vault::card::vikey {

// Owns the immutable, non-secret result of one atomic ViKey media probe.
// Route inspection reads only this snapshot and therefore cannot launch a
// second helper, change the bound hardware identity, or write the card.
class VikeyCardInsertionContext final
    : public application::IReadOnlyCardEntryInspection {
 public:
  explicit VikeyCardInsertionContext(VikeyCardMediaProbe probe);

  // Cache a same-card journal inspection after the legacy probe rejected its
  // format. Evidence is not authentication; callers must verify the complete
  // journal before supplying this state. Old target factories retain the
  // original unsupported probe and therefore cannot consume this as v1.
  VikeyCardInsertionContext(VikeyCardMediaProbe probe,
                            CardProductRecordState journal_state);

  [[nodiscard]] application::CardEntryMediaEvidence
  inspect_media() override;
  [[nodiscard]] bool destructive_reset_available() const noexcept override { return reset_available_; }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override;

  [[nodiscard]] CardProductRecordState
  inspect_supported_product_record_read_only() override;

  // The same snapshot is passed to the initialization target only after the
  // vendor-neutral router selects initialization_required.
  [[nodiscard]] const VikeyCardMediaProbe& probe() const noexcept;

 private:
  friend class VikeyHelperCardEnrollment;
  const VikeyCardMediaProbe probe_;
  const std::optional<CardProductRecordState> journal_state_;
  std::shared_ptr<const std::byte> insertion_identity_;
  bool reset_available_{};
};

}  // namespace usb_vault::card::vikey
