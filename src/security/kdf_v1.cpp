#include "usb_vault/security/kdf_v1.h"

#include "security/argon2id_runtime.h"

#include <stdexcept>

namespace usb_vault::security {

SecureBuffer derive_volume_password_v1(const SecureBuffer &password_utf8,
                                       const SecureBuffer &card_secret,
                                       const KdfV1Salt &salt) {
  if (card_secret.size() != kKdfV1CardSecretSize) {
    throw std::invalid_argument("card secret must contain exactly 32 bytes");
  }
  return detail::derive_argon2id(password_utf8, salt, card_secret.bytes(),
                                 kKdfV1DomainTag,
                                 {.memory_kib = kKdfV1MemoryKib,
                                  .iterations = kKdfV1Iterations,
                                  .parallelism = kKdfV1Parallelism},
                                 kKdfV1OutputSize);
}

} // namespace usb_vault::security
