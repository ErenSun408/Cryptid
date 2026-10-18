#pragma once

#include "usb_vault/card/card_record_v1.h"
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

// Selects and classifies the current logical product record without invoking
// journal normalization or any vendor write. Call only after the same session
// has classified the media as product_evidence.
[[nodiscard]] CardRecordState
inspect_vikey_card_record_state_read_only(VikeySession& session);

// Revalidates the exact adapter-authorized initial state in the same stable
// session, then establishes the recoverable provisioning journal. Only blank
// media and a clean interrupted bootstrap are eligible; every other state is
// rejected before a vendor write.
void bootstrap_vikey_card_media(VikeySession& session,
                                VikeyCardMediaState expected_state);

// Development-only destructive reset for the one ambiguous media shape left
// by P1: a nonzero 32-byte prefix and an otherwise blank user-data area. The
// caller must have explicitly selected that exact state. The implementation
// revalidates it in the same hardware-bound session, verifies zero writes over
// the complete user-data area, and clears the prefix last so an interrupted
// attempt remains either retryable legacy-shaped media or fully blank media.
// No product path may call this operation automatically.
void reset_vikey_development_legacy_media(
    VikeySession& session, VikeyCardMediaState expected_state);

[[nodiscard]] security::SecureBuffer read_vikey_card_record(
    VikeySession& session);

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record);

[[nodiscard]] security::SecureBuffer snapshot_vikey_card_record(VikeySession& session);
// Destructive reset after native confirmation. On cards large enough for the
// contact list it first verifies the confirmed snapshot, then zeroes and
// verifies [1808, 8192), then resets the journal.
void reset_vikey_card_record(VikeySession& session, const security::SecureBuffer& expected);

[[nodiscard]] security::SecureBuffer snapshot_stable_vikey_card_record(VikeySession& session);

// Restricted import: initialized v1 logical record to blank/provisioned media only.
// With contact_region (the source's whole [1808, 8192) ciphertext, P5.14) the
// region is written and verified after the target is authorized and
// provisioned but before the record is committed, so an interrupted copy
// leaves an uninitialized target the backup can be retried on. A target too
// small for the region is refused before any write.
void write_vikey_spare_card(VikeySession& session, const security::SecureBuffer& record,
                            const security::SecureBuffer* contact_region = nullptr);

// P5.14 contact list region [1808, 8192). Both operations require a card whose
// journal selects an initialized product record, so the helper never touches
// this range on blank, foreign or uninitialized media. A user area smaller
// than the region end fails with invalid_configuration at stage sdk_capacity.
// Ciphertext only: the helper neither holds the store key nor parses copies.
[[nodiscard]] security::SecureBuffer read_vikey_contact_region(
    VikeySession& session);

// Writes one whole copy (index 0 or 1) without readback; the application
// reads the region back through a separate session and decides success.
void write_vikey_contact_copy(VikeySession& session, std::size_t copy_index,
                              const security::SecureBuffer& copy);

}  // namespace usb_vault::card::vikey
