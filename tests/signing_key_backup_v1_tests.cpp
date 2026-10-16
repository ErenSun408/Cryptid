#include "usb_vault/licensing/signing_key_backup_v1.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using usb_vault::licensing::ActivationSigner;
using usb_vault::licensing::kSigningKeyBackupBytes;
using usb_vault::licensing::open_signing_key_backup;
using usb_vault::licensing::seal_signing_key_backup;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

}  // namespace

int main() {
  try {
    const ActivationSigner signer = ActivationSigner::generate();
    const SecureBuffer password = secure_text("备份口令 backup");
    const std::vector<std::byte> backup =
        seal_signing_key_backup(signer, password);
    require(backup.size() == kSigningKeyBackupBytes, "backup has the wrong size");

    const auto opened = open_signing_key_backup(backup, password);
    require(opened.has_value(), "the right password did not open the backup");
    require(opened->public_key() == signer.public_key(),
            "the opened key differs from the sealed key");

    require(!open_signing_key_backup(backup, secure_text("backup")),
            "a wrong password opened the backup");

    // The salt, nonce, ciphertext and tag are each covered: a flipped byte in
    // any of them changes the derived key or fails authentication.
    for (const std::size_t at : {std::size_t{0}, std::size_t{8},
                                 std::size_t{24}, std::size_t{36},
                                 kSigningKeyBackupBytes - 1}) {
      std::vector<std::byte> changed = backup;
      changed[at] ^= std::byte{1};
      require(!open_signing_key_backup(changed, password),
              "an altered backup opened");
    }
    std::vector<std::byte> shorter(backup.begin(), backup.end() - 1);
    require(!open_signing_key_backup(shorter, password),
            "a truncated backup opened");

    const std::vector<std::byte> again = seal_signing_key_backup(signer, password);
    require(again != backup, "two backups reused the same salt and nonce");
  } catch (const std::exception& error) {
    std::cerr << "signing_key_backup_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "signing_key_backup_v1_tests passed\n";
  return 0;
}
