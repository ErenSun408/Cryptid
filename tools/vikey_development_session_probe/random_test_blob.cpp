#include "random_test_blob.h"

#include "card/vikey/vikey_session.h"

#include <Windows.h>
#include <bcrypt.h>

#include <limits>
#include <system_error>

namespace usb_vault::card::vikey::development {

security::SecureBuffer random_test_blob() {
  security::SecureBuffer result(kVikeyBlobBytes);
  static_assert(kVikeyBlobBytes <= std::numeric_limits<ULONG>::max());
  const NTSTATUS status = ::BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(result.data()),
      static_cast<ULONG>(result.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            "generate development ViKey test blob");
  }
  return result;
}

}  // namespace usb_vault::card::vikey::development
