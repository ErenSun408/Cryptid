#pragma once

#include "card/vikey/vikey_session.h"

namespace usb_vault::card::vikey {

// Development-only destructive probe. Once the original blob has been read,
// every later path attempts to restore it and verifies the restored bytes.
// Failure to prove restoration is reported as device_failure.
void verify_development_blob_round_trip(
    VikeySession& session, const security::SecureBuffer& test_blob);

}  // namespace usb_vault::card::vikey
