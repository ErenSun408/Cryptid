#pragma once

#include "usb_vault/security/secure_buffer.h"

namespace usb_vault::card::vikey {

class VikeySession;

[[nodiscard]] security::SecureBuffer read_vikey_card_record(
    VikeySession& session);

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record);

}  // namespace usb_vault::card::vikey
