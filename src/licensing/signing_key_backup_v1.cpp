#include "usb_vault/licensing/signing_key_backup_v1.h"

#include "security/argon2id_runtime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace usb_vault::licensing {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'S'}, std::byte{'K'},
    std::byte{'B'}, std::byte{'1'}, std::byte{0},   std::byte{0}};
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kBlobBytes = 104;
constexpr std::size_t kTagBytes = 16;
constexpr std::size_t kSaltAt = kMagic.size();
constexpr std::size_t kNonceAt = kSaltAt + kSaltBytes;
constexpr std::size_t kSealedAt = kNonceAt + kNonceBytes;
constexpr std::size_t kTagAt = kSealedAt + kBlobBytes;
static_assert(kTagAt + kTagBytes == kSigningKeyBackupBytes);

constexpr std::string_view kDomain = "usb-vault:activation-key-backup:v1";
constexpr NTSTATUS kStatusAuthTagMismatch =
    static_cast<NTSTATUS>(0xC000A002UL);

[[noreturn]] void throw_status(const NTSTATUS status, const char* const what) {
  throw std::runtime_error(std::string(what) + " failed with NTSTATUS " +
                           std::to_string(static_cast<unsigned long>(status)));
}

[[nodiscard]] PUCHAR bytes_of(const std::byte* const data) noexcept {
  return reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data));
}

// One AES-256-GCM key derived from the password and salt; the handles close
// on every path, and CNG keeps the key schedule in its own object.
class SealingKey final {
 public:
  SealingKey(const security::SecureBuffer& password_utf8,
             const std::span<const std::byte> salt) {
    security::SecureBuffer key = security::detail::derive_argon2id(
        password_utf8, salt, {}, kDomain,
        {.memory_kib = 512U * 1024U, .iterations = 2, .parallelism = 1}, 32);
    NTSTATUS status =
        ::BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) throw_status(status, "open AES");
    status = ::BCryptSetProperty(
        algorithm_, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
    if (BCRYPT_SUCCESS(status)) {
      status = ::BCryptGenerateSymmetricKey(
          algorithm_, &key_, nullptr, 0, bytes_of(key.data()),
          static_cast<ULONG>(key.size()), 0);
    }
    if (!BCRYPT_SUCCESS(status)) {
      static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm_, 0));
      throw_status(status, "prepare AES-GCM");
    }
  }
  ~SealingKey() {
    static_cast<void>(::BCryptDestroyKey(key_));
    static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm_, 0));
  }
  SealingKey(const SealingKey&) = delete;
  SealingKey& operator=(const SealingKey&) = delete;

  [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return key_; }

 private:
  BCRYPT_ALG_HANDLE algorithm_{};
  BCRYPT_KEY_HANDLE key_{};
};

[[nodiscard]] BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO mode_info(
    const std::byte* const nonce, const std::byte* const tag) noexcept {
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = bytes_of(nonce);
  info.cbNonce = static_cast<ULONG>(kNonceBytes);
  info.pbAuthData = bytes_of(kMagic.data());
  info.cbAuthData = static_cast<ULONG>(kMagic.size());
  info.pbTag = bytes_of(tag);
  info.cbTag = static_cast<ULONG>(kTagBytes);
  return info;
}

}  // namespace

std::vector<std::byte> seal_signing_key_backup(
    const ActivationSigner& signer,
    const security::SecureBuffer& password_utf8) {
  if (signer.private_blob().size() != kBlobBytes) {
    throw std::logic_error("signing key is not loaded");
  }
  std::vector<std::byte> backup(kSigningKeyBackupBytes);
  std::ranges::copy(kMagic, backup.begin());
  const NTSTATUS random = ::BCryptGenRandom(
      nullptr, bytes_of(backup.data() + kSaltAt),
      static_cast<ULONG>(kSaltBytes + kNonceBytes),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(random)) throw_status(random, "BCryptGenRandom");

  const SealingKey key(password_utf8,
                       std::span(backup).subspan(kSaltAt, kSaltBytes));
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info =
      mode_info(backup.data() + kNonceAt, backup.data() + kTagAt);
  ULONG written = 0;
  const NTSTATUS status = ::BCryptEncrypt(
      key.get(), bytes_of(signer.private_blob().data()),
      static_cast<ULONG>(kBlobBytes), &info, nullptr, 0,
      bytes_of(backup.data() + kSealedAt), static_cast<ULONG>(kBlobBytes),
      &written, 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "seal the signing key");
  if (written != kBlobBytes) throw std::runtime_error("unexpected sealed size");
  return backup;
}

std::optional<ActivationSigner> open_signing_key_backup(
    const std::span<const std::byte> backup,
    const security::SecureBuffer& password_utf8) {
  if (backup.size() != kSigningKeyBackupBytes ||
      !std::ranges::equal(backup.first(kMagic.size()), kMagic)) {
    return std::nullopt;
  }
  const SealingKey key(password_utf8, backup.subspan(kSaltAt, kSaltBytes));
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info =
      mode_info(backup.data() + kNonceAt, backup.data() + kTagAt);
  security::SecureBuffer blob(kBlobBytes);
  ULONG written = 0;
  const NTSTATUS status = ::BCryptDecrypt(
      key.get(), bytes_of(backup.data() + kSealedAt),
      static_cast<ULONG>(kBlobBytes), &info, nullptr, 0,
      bytes_of(blob.data()), static_cast<ULONG>(blob.size()), &written, 0);
  if (status == kStatusAuthTagMismatch) return std::nullopt;
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "open the signing key");
  if (written != kBlobBytes) throw std::runtime_error("unexpected opened size");
  try {
    return ActivationSigner::import_private_blob(blob.bytes());
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

}  // namespace usb_vault::licensing
