#include "usb_vault/licensing/activation_code_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace usb_vault::licensing {
namespace {

constexpr std::uint8_t kFormatVersion = 1;
constexpr std::size_t kPayloadBytes = 14;
constexpr std::size_t kCoordinateBytes = 32;
constexpr std::size_t kSignatureBytes = 2 * kCoordinateBytes;
constexpr std::size_t kCodeBytes = kPayloadBytes + kSignatureBytes;
constexpr std::size_t kCodeCharacters = (kCodeBytes * 8 + 4) / 5;
constexpr std::size_t kGroupCharacters = 5;
constexpr std::size_t kPrivateBlobBytes =
    sizeof(BCRYPT_ECCKEY_BLOB) + 3 * kCoordinateBytes;
// 9999-12-31T23:59:59Z: the tool cannot express a later instant, so a later
// value can only come from a forged or corrupted code.
constexpr std::int64_t kLatestExpiry = 253402300799;
constexpr std::string_view kDomain = "usb-vault activation v1";
constexpr std::string_view kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

using Payload = std::array<std::byte, kPayloadBytes>;
using Digest = std::array<std::byte, 32>;

[[noreturn]] void throw_status(const NTSTATUS status, const char* const what) {
  throw std::runtime_error(std::string(what) + " failed with NTSTATUS " +
                           std::to_string(static_cast<unsigned long>(status)));
}

class Algorithm final {
 public:
  explicit Algorithm(const wchar_t* const id) {
    const NTSTATUS status =
        ::BCryptOpenAlgorithmProvider(&handle_, id, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) throw_status(status, "BCryptOpenAlgorithmProvider");
  }
  ~Algorithm() { static_cast<void>(::BCryptCloseAlgorithmProvider(handle_, 0)); }
  Algorithm(const Algorithm&) = delete;
  Algorithm& operator=(const Algorithm&) = delete;
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class Key final {
 public:
  Key() noexcept = default;
  ~Key() {
    if (handle_ != nullptr) static_cast<void>(::BCryptDestroyKey(handle_));
  }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] BCRYPT_KEY_HANDLE* put() noexcept { return &handle_; }

 private:
  BCRYPT_KEY_HANDLE handle_{};
};

[[nodiscard]] Payload payload_of(const ActivationGrant& grant) {
  Payload payload{};
  payload[0] = std::byte{kFormatVersion};
  payload[1] = std::byte{kActivationKeyId};
  for (int i = 0; i < 4; ++i) {
    payload[2 + i] =
        static_cast<std::byte>(grant.card_hardware_id >> (8 * (3 - i)));
  }
  const auto expiry = static_cast<std::uint64_t>(grant.expires_at_utc_seconds);
  for (int i = 0; i < 8; ++i) {
    payload[6 + i] = static_cast<std::byte>(expiry >> (8 * (7 - i)));
  }
  return payload;
}

[[nodiscard]] Digest digest_of(const Payload& payload) {
  Algorithm sha256(BCRYPT_SHA256_ALGORITHM);
  BCRYPT_HASH_HANDLE hash{};
  NTSTATUS status = ::BCryptCreateHash(sha256.get(), &hash, nullptr, 0,
                                       nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "BCryptCreateHash");
  Digest digest{};
  // The terminating zero separates the domain from the payload.
  status = ::BCryptHashData(
      hash, reinterpret_cast<PUCHAR>(const_cast<char*>(kDomain.data())),
      static_cast<ULONG>(kDomain.size() + 1), 0);
  if (BCRYPT_SUCCESS(status)) {
    status = ::BCryptHashData(
        hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(payload.data())),
        static_cast<ULONG>(payload.size()), 0);
  }
  if (BCRYPT_SUCCESS(status)) {
    status = ::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                                static_cast<ULONG>(digest.size()), 0);
  }
  static_cast<void>(::BCryptDestroyHash(hash));
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "SHA-256");
  return digest;
}

[[nodiscard]] std::string encode(const std::array<std::byte, kCodeBytes>& bytes) {
  std::string symbols;
  symbols.reserve(kCodeCharacters);
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

  std::string text;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    if (i != 0 && i % kGroupCharacters == 0) text += '-';
    text += symbols[i];
  }
  return text;
}

[[nodiscard]] int symbol_value(char symbol) noexcept {
  if (symbol >= 'a' && symbol <= 'z') symbol = static_cast<char>(symbol - 'a' + 'A');
  if (symbol == 'O') symbol = '0';
  if (symbol == 'I' || symbol == 'L') symbol = '1';
  const std::size_t at = kAlphabet.find(symbol);
  return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

[[nodiscard]] std::optional<std::array<std::byte, kCodeBytes>> decode(
    const std::string_view text) noexcept {
  std::array<std::byte, kCodeBytes> bytes{};
  std::size_t count = 0;
  std::size_t written = 0;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char symbol : text) {
    if (symbol == '-' || symbol == ' ' || symbol == '\t' || symbol == '\r' ||
        symbol == '\n') {
      continue;
    }
    const int value = symbol_value(symbol);
    if (value < 0 || ++count > kCodeCharacters) return std::nullopt;
    buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (written == bytes.size()) return std::nullopt;
      bytes[written++] = static_cast<std::byte>((buffer >> bits) & 0xFF);
    }
  }
  // Exactly one canonical spelling: every byte present and the padding zero.
  if (count != kCodeCharacters || written != bytes.size()) return std::nullopt;
  if ((buffer & ((1U << bits) - 1)) != 0) return std::nullopt;
  return bytes;
}

[[nodiscard]] Algorithm ecdsa() { return Algorithm(BCRYPT_ECDSA_P256_ALGORITHM); }

[[nodiscard]] bool leap_year(const int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

// Days since 1970-01-01 in the proleptic Gregorian calendar.
[[nodiscard]] std::int64_t days_from_civil(int year, const int month,
                                           const int day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<std::int64_t>(year - era * 400);
  const std::int64_t day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const std::int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

}  // namespace

std::optional<ActivationGrant> verify_activation_code(
    const std::string_view code, const ActivationPublicKey& public_key) {
  const auto bytes = decode(code);
  if (!bytes) return std::nullopt;
  if ((*bytes)[0] != std::byte{kFormatVersion} ||
      (*bytes)[1] != std::byte{kActivationKeyId}) {
    return std::nullopt;
  }

  ActivationGrant grant;
  for (int i = 0; i < 4; ++i) {
    grant.card_hardware_id = (grant.card_hardware_id << 8) |
                             std::to_integer<std::uint32_t>((*bytes)[2 + i]);
  }
  std::uint64_t expiry = 0;
  for (int i = 0; i < 8; ++i) {
    expiry = (expiry << 8) | std::to_integer<std::uint64_t>((*bytes)[6 + i]);
  }
  if (expiry == 0 || expiry > static_cast<std::uint64_t>(kLatestExpiry)) {
    return std::nullopt;
  }
  grant.expires_at_utc_seconds = static_cast<std::int64_t>(expiry);

  std::array<std::byte, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoordinateBytes>
      public_blob{};
  const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC,
                                  static_cast<ULONG>(kCoordinateBytes)};
  std::memcpy(public_blob.data(), &header, sizeof(header));
  std::ranges::copy(public_key, public_blob.begin() + sizeof(header));

  const Algorithm algorithm = ecdsa();
  Key key;
  if (!BCRYPT_SUCCESS(::BCryptImportKeyPair(
          algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, key.put(),
          reinterpret_cast<PUCHAR>(public_blob.data()),
          static_cast<ULONG>(public_blob.size()), 0))) {
    return std::nullopt;
  }

  Payload payload{};
  std::copy_n(bytes->begin(), kPayloadBytes, payload.begin());
  Digest digest = digest_of(payload);
  std::array<std::byte, kSignatureBytes> signature{};
  std::copy_n(bytes->begin() + kPayloadBytes, kSignatureBytes,
              signature.begin());
  const NTSTATUS status = ::BCryptVerifySignature(
      key.get(), nullptr, reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      reinterpret_cast<PUCHAR>(signature.data()),
      static_cast<ULONG>(signature.size()), 0);
  if (!BCRYPT_SUCCESS(status)) return std::nullopt;
  return grant;
}

ActivationSigner ActivationSigner::generate() {
  const Algorithm algorithm = ecdsa();
  Key key;
  NTSTATUS status = ::BCryptGenerateKeyPair(
      algorithm.get(), key.put(), static_cast<ULONG>(8 * kCoordinateBytes), 0);
  if (BCRYPT_SUCCESS(status)) status = ::BCryptFinalizeKeyPair(key.get(), 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "generate the signing key");

  security::SecureBuffer blob(kPrivateBlobBytes);
  ULONG written = 0;
  status = ::BCryptExportKey(key.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB,
                             reinterpret_cast<PUCHAR>(blob.data()),
                             static_cast<ULONG>(blob.size()), &written, 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "export the signing key");
  if (written != blob.size()) throw std::runtime_error("unexpected signing key size");
  return ActivationSigner(std::move(blob));
}

ActivationSigner ActivationSigner::import_private_blob(
    const std::span<const std::byte> blob) {
  BCRYPT_ECCKEY_BLOB header{};
  if (blob.size() != kPrivateBlobBytes) {
    throw std::invalid_argument("signing key has the wrong size");
  }
  std::memcpy(&header, blob.data(), sizeof(header));
  if (header.dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC ||
      header.cbKey != kCoordinateBytes) {
    throw std::invalid_argument("signing key is not an ECDSA P-256 key");
  }
  security::SecureBuffer copy(blob.size());
  std::ranges::copy(blob, copy.bytes().begin());

  // Importing proves that CNG accepts the key before it is ever used to sign.
  const Algorithm algorithm = ecdsa();
  Key key;
  const NTSTATUS status = ::BCryptImportKeyPair(
      algorithm.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB, key.put(),
      reinterpret_cast<PUCHAR>(copy.data()), static_cast<ULONG>(copy.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::invalid_argument("signing key was rejected");
  }
  // CNG does not promise that X, Y and d belong together; a mismatched key
  // would publish a public key that rejects every code it signs.
  ActivationSigner signer(std::move(copy));
  constexpr ActivationGrant kProbe{0, 1};
  if (verify_activation_code(signer.sign(kProbe), signer.public_key()) !=
      kProbe) {
    throw std::invalid_argument("signing key does not match its public key");
  }
  return signer;
}

ActivationPublicKey ActivationSigner::public_key() const {
  if (private_blob_.size() != kPrivateBlobBytes) {
    throw std::logic_error("signing key is not loaded");
  }
  ActivationPublicKey result{};
  std::copy_n(private_blob_.bytes().begin() + sizeof(BCRYPT_ECCKEY_BLOB),
              result.size(), result.begin());
  return result;
}

std::string ActivationSigner::sign(const ActivationGrant& grant) const {
  if (grant.expires_at_utc_seconds <= 0 ||
      grant.expires_at_utc_seconds > kLatestExpiry) {
    throw std::invalid_argument("expiry is outside the code range");
  }
  const Algorithm algorithm = ecdsa();
  Key key;
  NTSTATUS status = ::BCryptImportKeyPair(
      algorithm.get(), nullptr, BCRYPT_ECCPRIVATE_BLOB, key.put(),
      reinterpret_cast<PUCHAR>(const_cast<std::byte*>(private_blob_.data())),
      static_cast<ULONG>(private_blob_.size()), 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "import the signing key");

  const Payload payload = payload_of(grant);
  Digest digest = digest_of(payload);
  std::array<std::byte, kCodeBytes> bytes{};
  std::ranges::copy(payload, bytes.begin());
  ULONG written = 0;
  status = ::BCryptSignHash(
      key.get(), nullptr, reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      reinterpret_cast<PUCHAR>(bytes.data() + kPayloadBytes),
      static_cast<ULONG>(kSignatureBytes), &written, 0);
  if (!BCRYPT_SUCCESS(status)) throw_status(status, "BCryptSignHash");
  if (written != kSignatureBytes) throw std::runtime_error("unexpected signature size");
  return encode(bytes);
}

std::optional<std::int64_t> utc_seconds_from_offset_time(
    const int year, const int month, const int day, const int hour,
    const int minute, const int second, const int offset_minutes) noexcept {
  constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1) {
    return std::nullopt;
  }
  const int month_days =
      kDaysInMonth[month - 1] + (month == 2 && leap_year(year) ? 1 : 0);
  if (day > month_days || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    return std::nullopt;
  }
  if (offset_minutes < -12 * 60 || offset_minutes > 14 * 60 ||
      offset_minutes % 15 != 0) {
    return std::nullopt;
  }
  const std::int64_t local = days_from_civil(year, month, day) * 86400 +
                             hour * 3600 + minute * 60 + second;
  return local - static_cast<std::int64_t>(offset_minutes) * 60;
}

ActivationState evaluate_activation(
    const std::optional<ActivationGrant>& grant,
    const std::uint32_t card_hardware_id, const std::int64_t now_utc_seconds,
    const std::int64_t highest_observed_utc_seconds) noexcept {
  if (!grant || grant->card_hardware_id != card_hardware_id) {
    return ActivationState::missing;
  }
  // A clock at or before 1970 is not a reading to trust. Comparing against
  // the lowered high-water mark avoids overflow at either end of the range.
  if (now_utc_seconds <= 0 ||
      (highest_observed_utc_seconds > kClockRewindToleranceSeconds &&
       now_utc_seconds <
           highest_observed_utc_seconds - kClockRewindToleranceSeconds)) {
    return ActivationState::clock_rewound;
  }
  return now_utc_seconds < grant->expires_at_utc_seconds
             ? ActivationState::active
             : ActivationState::expired;
}

}  // namespace usb_vault::licensing
