#pragma once

#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::card::vikey {

class VikeySession;

// ViKey-private, read-only classification of the complete user-data area.
// A 32-byte prefix cannot be proven to be a P1 blob because P1 had no magic;
// that state therefore remains blocked together with same-shaped foreign data.
enum class VikeyCardMediaState {
  blank,
  legacy_p1_or_foreign_prefix,
  foreign_data,
  provisioning_interrupted,
  conflicting_evidence,
  product_evidence,
};

[[nodiscard]] VikeyCardMediaState inspect_vikey_card_media(
    VikeySession& session);

// Revalidates the exact adapter-authorized initial state in the same stable
// session, then establishes the recoverable provisioning journal. Only blank
// media and a clean interrupted bootstrap are eligible; every other state is
// rejected before a vendor write.
void bootstrap_vikey_card_media(VikeySession& session,
                                VikeyCardMediaState expected_state);

[[nodiscard]] security::SecureBuffer read_vikey_card_record(
    VikeySession& session);

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record);

}  // namespace usb_vault::card::vikey
