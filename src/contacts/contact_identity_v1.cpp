#include "usb_vault/contacts/contact_identity_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace usb_vault::contacts {
namespace {

constexpr std::string_view kFingerprintDomain =
    "usb-vault:contact-fingerprint:v1";
constexpr std::string_view kKeyFileDomain = "usb-vault:contact-key-file:v1";
constexpr std::string_view kKeyFileHeaderPrefix = "USB-VAULT-CONTACT-KEY v";
constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kCardSecretSize = 32;
constexpr std::size_t kChecksumSize = 4;
constexpr std::size_t kKeyFileBytes = kContactPublicKeySize + kChecksumSize;
constexpr std::size_t kKeyFileSymbols = (kKeyFileBytes * 8 + 4) / 5;
constexpr std::size_t kKeyFileGroup = 5;
constexpr std::size_t kFingerprintGroup = 4;

using Digest = std::array<std::byte, 32>;

[[noreturn]] void throw_ntstatus(const NTSTATUS status,
                                 const char* const operation) {
  throw std::system_error(static_cast<int>(status), std::system_category(),
                          operation);
}

void require_success(const NTSTATUS status, const char* const operation) {
  if (!BCRYPT_SUCCESS(status)) throw_ntstatus(status, operation);
}

class Algorithm final {
 public:
  explicit Algorithm(const ULONG flags) {
    require_success(::BCryptOpenAlgorithmProvider(
                        &handle_, BCRYPT_SHA256_ALGORITHM, nullptr, flags),
                    flags == 0 ? "open SHA-256 provider"
                               : "open SHA-256 HMAC provider");
  }
  ~Algorithm() { static_cast<void>(::BCryptCloseAlgorithmProvider(handle_, 0)); }
  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

[[nodiscard]] ULONG object_length(const BCRYPT_ALG_HANDLE algorithm) {
  ULONG value = 0;
  ULONG written = 0;
  require_success(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&value),
                                      sizeof(value), &written, 0),
                  "read CNG object length");
  if (written != sizeof(value) || value == 0) {
    throw std::runtime_error("invalid CNG object length");
  }
  return value;
}

// The hash object lives in page-locked memory: for an HMAC keyed with S it
// holds key state from which any HMAC(S, .) could be computed (R-03).
class Hash final {
 public:
  Hash(const Algorithm& algorithm, std::span<const std::byte> key)
      : object_(object_length(algorithm.get())) {
    require_success(
        ::BCryptCreateHash(algorithm.get(), &handle_,
                           reinterpret_cast<PUCHAR>(object_.data()),
                           static_cast<ULONG>(object_.size()),
                           reinterpret_cast<PUCHAR>(
                               const_cast<std::byte*>(key.data())),
                           static_cast<ULONG>(key.size()), 0),
        "create SHA-256 hash");
  }
  ~Hash() { static_cast<void>(::BCryptDestroyHash(handle_)); }
  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;

  void update(std::span<const std::byte> data) {
    require_success(
        ::BCryptHashData(handle_,
                         reinterpret_cast<PUCHAR>(
                             const_cast<std::byte*>(data.data())),
                         static_cast<ULONG>(data.size()), 0),
        "hash data");
  }
  void update(const std::string_view text) {
    update(std::as_bytes(std::span(text.data(), text.size())));
  }
  void finish(std::span<std::byte> out) {
    require_success(::BCryptFinishHash(handle_,
                                       reinterpret_cast<PUCHAR>(out.data()),
                                       static_cast<ULONG>(out.size()), 0),
                    "finish hash");
  }

 private:
  // Declared first so the handle is destroyed before its object memory.
  security::SecureBuffer object_;
  BCRYPT_HASH_HANDLE handle_{};
};

[[nodiscard]] Digest domain_digest(const std::string_view domain,
                                   const ContactPublicKey& key) {
  const Algorithm sha256(0);
  Hash hash(sha256, {});
  hash.update(domain);
  hash.update(key);
  Digest digest{};
  hash.finish(digest);
  return digest;
}

[[nodiscard]] const std::uint8_t* u8(const std::byte* const bytes) noexcept {
  return reinterpret_cast<const std::uint8_t*>(bytes);
}
[[nodiscard]] std::uint8_t* u8(std::byte* const bytes) noexcept {
  return reinterpret_cast<std::uint8_t*>(bytes);
}

[[nodiscard]] std::string base32(std::span<const std::byte> bytes) {
  std::string symbols;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const std::byte value : bytes) {
    buffer = (buffer << 8) | std::to_integer<std::uint32_t>(value);
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      symbols += kAlphabet[(buffer >> bits) & 0x1F];
    }
  }
  if (bits > 0) symbols += kAlphabet[(buffer << (5 - bits)) & 0x1F];
  return symbols;
}

[[nodiscard]] std::string grouped(const std::string& symbols,
                                  const std::size_t group) {
  std::string text;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    if (i != 0 && i % group == 0) text += '-';
    text += symbols[i];
  }
  return text;
}

[[nodiscard]] int symbol_value(char symbol) noexcept {
  if (symbol >= 'a' && symbol <= 'z') {
    symbol = static_cast<char>(symbol - 'a' + 'A');
  }
  if (symbol == 'O') symbol = '0';
  if (symbol == 'I' || symbol == 'L') symbol = '1';
  const std::size_t at = kAlphabet.find(symbol);
  return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

[[nodiscard]] bool is_blank(const char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim_blanks(std::string_view text) noexcept {
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);
  return text;
}

// Strict: exactly the expected symbol count and zero padding bits, so each
// payload has one canonical symbol sequence apart from case and aliases.
[[nodiscard]] std::optional<std::array<std::byte, kKeyFileBytes>> decode_key(
    const std::string_view line) {
  std::array<std::byte, kKeyFileBytes> out{};
  std::size_t written = 0;
  std::size_t symbols = 0;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : line) {
    if (c == '-' || c == ' ' || c == '\t') continue;
    const int value = symbol_value(c);
    if (value < 0 || ++symbols > kKeyFileSymbols) return std::nullopt;
    buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out[written++] = static_cast<std::byte>((buffer >> bits) & 0xFF);
    }
  }
  if (symbols != kKeyFileSymbols || written != kKeyFileBytes) {
    return std::nullopt;
  }
  if ((buffer & ((1U << bits) - 1U)) != 0) return std::nullopt;
  return out;
}

// Ed25519 encodes y little-endian in the low 255 bits; values at or above
// p = 2^255 - 19 are non-canonical aliases of a smaller y.
[[nodiscard]] bool canonical_y(const ContactPublicKey& key) noexcept {
  if ((std::to_integer<unsigned>(key[31]) & 0x7F) != 0x7F) return true;
  for (std::size_t i = 30; i >= 1; --i) {
    if (key[i] != std::byte{0xFF}) return true;
  }
  return std::to_integer<unsigned>(key[0]) < 0xED;
}

[[nodiscard]] bool is_version_number(const std::string_view suffix) noexcept {
  return !suffix.empty() &&
         std::all_of(suffix.begin(), suffix.end(),
                     [](const char c) { return c >= '0' && c <= '9'; });
}

[[nodiscard]] std::array<std::byte, kChecksumSize> checksum_of(
    const ContactPublicKey& key) {
  const Digest digest = domain_digest(kKeyFileDomain, key);
  std::array<std::byte, kChecksumSize> checksum{};
  std::copy_n(digest.begin(), kChecksumSize, checksum.begin());
  return checksum;
}

}  // namespace

ContactIdentity ContactIdentity::derive(
    const security::SecureBuffer& card_secret) {
  if (card_secret.size() != kCardSecretSize) {
    throw std::invalid_argument("contact identity requires a 32-byte secret");
  }

  security::SecureBuffer seed(32);
  {
    const Algorithm hmac(BCRYPT_ALG_HANDLE_HMAC_FLAG);
    Hash hash(hmac, card_secret.bytes());
    hash.update(kContactIdentityDomain);
    hash.finish(seed.bytes());
  }

  // Monocypher derives the X25519 secret from the seed's SHA-512, matching
  // the Ed25519 secret scalar; key_pair wipes the seed it is given.
  security::SecureBuffer seed_hash(64);
  crypto_sha512(u8(seed_hash.data()), u8(seed.data()), seed.size());
  security::SecureBuffer x25519_secret(32);
  std::memcpy(x25519_secret.data(), seed_hash.data(), x25519_secret.size());
  seed_hash.clear();

  security::SecureBuffer ed25519_secret(64);
  ContactPublicKey public_key{};
  crypto_ed25519_key_pair(u8(ed25519_secret.data()), u8(public_key.data()),
                          u8(seed.data()));

  ContactX25519PublicKey x25519_public{};
  crypto_x25519_public_key(u8(x25519_public.data()),
                           u8(x25519_secret.data()));
  return ContactIdentity(std::move(ed25519_secret), std::move(x25519_secret),
                         public_key, x25519_public);
}

ContactSignature ContactIdentity::sign(
    std::span<const std::byte> message) const {
  if (ed25519_secret_.empty()) {
    throw std::logic_error("contact identity was moved from");
  }
  ContactSignature signature{};
  crypto_ed25519_sign(u8(signature.data()), u8(ed25519_secret_.data()),
                      u8(message.data()), message.size());
  return signature;
}

std::optional<security::SecureBuffer> ContactIdentity::agree(
    const ContactX25519PublicKey& peer) const {
  if (x25519_secret_.empty()) {
    throw std::logic_error("contact identity was moved from");
  }
  security::SecureBuffer shared(32);
  crypto_x25519(u8(shared.data()), u8(x25519_secret_.data()), u8(peer.data()));
  constexpr std::array<std::uint8_t, 32> kZero{};
  if (crypto_verify32(u8(shared.data()), kZero.data()) == 0) {
    return std::nullopt;
  }
  return shared;
}

bool verify_contact_signature(const ContactPublicKey& public_key,
                              std::span<const std::byte> message,
                              const ContactSignature& signature) noexcept {
  // Monocypher's cofactored check accepts any signature under a low-order
  // key and allows non-canonical encodings, so both are refused first.
  if (!contact_public_key_usable(public_key)) return false;
  return crypto_ed25519_check(u8(signature.data()), u8(public_key.data()),
                              u8(message.data()), message.size()) == 0;
}

ContactX25519PublicKey contact_x25519_public_key(
    const ContactPublicKey& public_key) noexcept {
  ContactX25519PublicKey out{};
  crypto_eddsa_to_x25519(u8(out.data()), u8(public_key.data()));
  return out;
}

bool contact_public_key_usable(const ContactPublicKey& public_key) noexcept {
  if (!canonical_y(public_key)) return false;
  // X25519 clamps every scalar to a multiple of the cofactor, so the product
  // is zero exactly when the point is low order; the scalar value is
  // otherwise irrelevant.
  constexpr std::array<std::uint8_t, 32> kProbeScalar{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
      0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
      0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
  constexpr std::array<std::uint8_t, 32> kZero{};
  const ContactX25519PublicKey x25519 = contact_x25519_public_key(public_key);
  std::array<std::uint8_t, 32> product{};
  crypto_x25519(product.data(), kProbeScalar.data(), u8(x25519.data()));
  return crypto_verify32(product.data(), kZero.data()) != 0;
}

std::array<std::byte, kContactFingerprintSize> contact_fingerprint(
    const ContactPublicKey& public_key) {
  const Digest digest = domain_digest(kFingerprintDomain, public_key);
  std::array<std::byte, kContactFingerprintSize> fingerprint{};
  std::copy_n(digest.begin(), kContactFingerprintSize, fingerprint.begin());
  return fingerprint;
}

std::string format_contact_fingerprint(const ContactPublicKey& public_key) {
  return grouped(base32(contact_fingerprint(public_key)), kFingerprintGroup);
}

std::string encode_contact_key_file(const ContactPublicKey& public_key) {
  std::array<std::byte, kKeyFileBytes> payload{};
  std::copy(public_key.begin(), public_key.end(), payload.begin());
  const auto checksum = checksum_of(public_key);
  std::copy(checksum.begin(), checksum.end(),
            payload.begin() + kContactPublicKeySize);

  std::string text(kContactKeyFileHeader);
  text += "\r\n";
  text += grouped(base32(payload), kKeyFileGroup);
  text += "\r\n";
  return text;
}

ContactKeyFileDecoding decode_contact_key_file(std::string_view text) {
  ContactKeyFileDecoding result;
  if (text.size() > kContactKeyFileMaxBytes) return result;
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

  const std::size_t header_end = text.find('\n');
  if (header_end == std::string_view::npos) return result;
  const std::string_view header = trim_blanks(text.substr(0, header_end));
  if (header != kContactKeyFileHeader) {
    if (header.starts_with(kKeyFileHeaderPrefix) &&
        is_version_number(header.substr(kKeyFileHeaderPrefix.size()))) {
      result.error = ContactKeyFileError::unsupported_version;
    }
    return result;
  }

  const std::optional<std::array<std::byte, kKeyFileBytes>> payload =
      decode_key(trim_blanks(text.substr(header_end + 1)));
  if (!payload) return result;

  ContactPublicKey key{};
  std::copy_n(payload->begin(), kContactPublicKeySize, key.begin());
  const auto checksum = checksum_of(key);
  if (!std::equal(checksum.begin(), checksum.end(),
                  payload->begin() + kContactPublicKeySize)) {
    result.error = ContactKeyFileError::checksum_mismatch;
    return result;
  }
  if (!contact_public_key_usable(key)) {
    result.error = ContactKeyFileError::unusable_key;
    return result;
  }
  result.public_key = key;
  result.error = ContactKeyFileError::none;
  return result;
}

}  // namespace usb_vault::contacts
