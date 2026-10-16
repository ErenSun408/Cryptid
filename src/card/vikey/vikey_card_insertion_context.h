#pragma once

#include "card/vikey/vikey_helper_client.h"
#include "usb_vault/application/card_entry_router.h"

namespace usb_vault::card::vikey {

// Owns the immutable, non-secret result of one atomic ViKey media probe.
// Route inspection reads only this snapshot and therefore cannot launch a
// second helper, change the bound hardware identity, or write the card.
class VikeyCardInsertionContext final
    : public application::IReadOnlyCardEntryInspection {
 public:
  explicit VikeyCardInsertionContext(VikeyCardMediaProbe probe);

  [[nodiscard]] application::CardEntryMediaEvidence
  inspect_media() override;

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override;

  [[nodiscard]] std::optional<std::uint32_t> card_identity()
      const noexcept override;

  // The same snapshot is passed to the initialization target only after the
  // vendor-neutral router selects initialization_required.
  [[nodiscard]] const VikeyCardMediaProbe& probe() const noexcept;

 private:
  const VikeyCardMediaProbe probe_;
};

}  // namespace usb_vault::card::vikey
