#include "usb_vault/card/card_record_v1.h"

#include "security/argon2id_runtime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace usb_vault::card {
namespace {

constexpr std::array<std::byte, 8> kInitializedMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
    std::byte{'D'}, std::byte{'2'}, std::byte{0},   std::byte{0}};
constexpr std::array<std::byte, 8> kProvisioningMagic{
    std::byte{'U'}, std::byte{'V'}, std::byte{'B'}, std::byte{'L'},
    std::byte{'N'}, std::byte{'K'}, std::byte{'2'}, std::byte{0}};
constexpr std::array<std::byte, 16> kPayloadMarker{
    std::byte{'U'}, std::byte{'V'}, std::byte{'R'}, std::byte{'O'},
    std::byte{'O'}, std::byte{'T'}, std::byte{'S'}, std::byte{'E'},
    std::byte{'C'}, std::byte{'R'}, std::byte{'E'}, std::byte{'T'},
    std::byte{'-'}, std::byte{'V'}, std::byte{'1'}, std::byte{0}};
constexpr std::string_view kCardMasterDomain =
    "usb-vault:card-record-master:v1";
constexpr std::string_view kEnvelopeKeyDomain =
    "usb-vault:card-envelope-key:v1";

constexpr std::uint32_t kCardMemoryKib = 512U * 1024U;
constexpr std::uint32_t kCardIterations = 2;
constexpr std::uint32_t kCardParallelism = 1;
constexpr std::uint32_t kArgon2Version13 = 0x13;
constexpr std::size_t kSaltOffset = 16;
constexpr std::size_t kSaltSize = 16;
constexpr std::size_t kRecordIdOffset = 32;
constexpr std::size_t kRecordIdSize = 16;
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kPlaintextSize = 48;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kMasterKeySize = 32;
constexpr std::size_t kAadSize = kCardRecordV1HeaderSize + 1;
constexpr std::size_t kHmacInputSize =
    kEnvelopeKeyDomain.size() + kRecordIdSize + 1;
constexpr NTSTATUS kStatusAuthTagMismatch = static_cast<NTSTATUS>(0xC000A002UL);

static_assert(kCardRecordV1PrimaryEnvelopeOffset == kCardRecordV1HeaderSize);
static_assert(kCardRecordV1SecondaryEnvelopeOffset ==
              kCardRecordV1PrimaryEnvelopeOffset + kCardRecordV1EnvelopeSize);
static_assert(kCardRecordV1Size ==
              kCardRecordV1HeaderSize + (2 * kCardRecordV1EnvelopeSize));
static_assert(kCardRecordV1EnvelopeSize ==
              kNonceSize + kPlaintextSize + kTagSize);

[[noreturn]] void throw_ntstatus(const NTSTATUS status,
                                 const char *const operation) {
  throw std::system_error(static_cast<int>(status), std::system_category(),
                          operation);
}

void require_success(const NTSTATUS status, const char *const operation) {
  if (!BCRYPT_SUCCESS(status)) {
    throw_ntstatus(status, operation);
  }
}

template <typename Handle, auto Close> class UniqueHandle final {
public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(Handle handle) noexcept : handle_(handle) {}
  ~UniqueHandle() noexcept { reset(); }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  UniqueHandle(UniqueHandle &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] Handle get() const noexcept { return handle_; }
  [[nodiscard]] Handle *put() noexcept {
    reset();
    return &handle_;
  }

private:
  void reset() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(Close(handle_));
      handle_ = nullptr;
    }
  }

  Handle handle_{nullptr};
};

NTSTATUS close_algorithm(const BCRYPT_ALG_HANDLE handle) noexcept {
  return ::BCryptCloseAlgorithmProvider(handle, 0);
}

using AlgorithmHandle = UniqueHandle<BCRYPT_ALG_HANDLE, close_algorithm>;
using HashHandle = UniqueHandle<BCRYPT_HASH_HANDLE, ::BCryptDestroyHash>;
using KeyHandle = UniqueHandle<BCRYPT_KEY_HANDLE, ::BCryptDestroyKey>;

[[nodiscard]] ULONG checked_ulong(const std::size_t size) {
  if (size > std::numeric_limits<ULONG>::max()) {
    throw std::length_error("CNG input is too large");
  }
  return static_cast<ULONG>(size);
}

[[nodiscard]] PUCHAR
mutable_bytes(const std::span<const std::byte> bytes) noexcept {
  const auto *const data = reinterpret_cast<const UCHAR *>(bytes.data());
  return const_cast<PUCHAR>(data);
}

[[nodiscard]] PUCHAR
mutable_bytes(const security::SecureBuffer &bytes) noexcept {
  return mutable_bytes(bytes.bytes());
}

void fill_random(const std::span<std::byte> destination) {
  require_success(
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(destination.data()),
                        checked_ulong(destination.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG),
      "BCryptGenRandom");
}

[[nodiscard]] bool
constant_time_equal(const std::span<const std::byte> left,
                    const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  unsigned int difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0;
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void write_u32(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
  bytes[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xffU);
  bytes[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] bool
exact_provisioning_marker(const std::span<const std::byte> record) noexcept {
  if (record.size() != kCardRecordV1Size ||
      !std::ranges::equal(record.first(kProvisioningMagic.size()),
                          kProvisioningMagic)) {
    return false;
  }
  return std::ranges::all_of(
      record.subspan(kProvisioningMagic.size()),
      [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] bool
valid_v1_header(const std::span<const std::byte> record) noexcept {
  return record.size() == kCardRecordV1Size &&
         std::ranges::equal(record.first(kInitializedMagic.size()),
                            kInitializedMagic) &&
         read_u32(record, 8) == kCardRecordV1Version &&
         read_u32(record, 12) == kCardRecordV1Size &&
         read_u32(record, 48) == kCardMemoryKib &&
         read_u32(record, 52) == kCardIterations &&
         read_u32(record, 56) == kCardParallelism &&
         read_u32(record, 60) == kArgon2Version13;
}

void write_header(const std::span<std::byte> record) {
  std::ranges::copy(kInitializedMagic, record.begin());
  write_u32(record, 8, kCardRecordV1Version);
  write_u32(record, 12, static_cast<std::uint32_t>(kCardRecordV1Size));
  fill_random(record.subspan(kSaltOffset, kSaltSize));
  fill_random(record.subspan(kRecordIdOffset, kRecordIdSize));
  write_u32(record, 48, kCardMemoryKib);
  write_u32(record, 52, kCardIterations);
  write_u32(record, 56, kCardParallelism);
  write_u32(record, 60, kArgon2Version13);
}

[[nodiscard]] security::SecureBuffer
derive_master_key(const security::SecureBuffer &password_utf8,
                  const std::span<const std::byte> record) {
  return security::detail::derive_argon2id(
      password_utf8, record.subspan(kSaltOffset, kSaltSize), {},
      kCardMasterDomain,
      {.memory_kib = kCardMemoryKib,
       .iterations = kCardIterations,
       .parallelism = kCardParallelism},
      kMasterKeySize);
}

[[nodiscard]] AlgorithmHandle open_sha256_hmac() {
  AlgorithmHandle algorithm;
  require_success(
      ::BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
                                    nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
      "open SHA-256 HMAC provider");
  return algorithm;
}

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

[[nodiscard]] security::SecureBuffer
derive_envelope_key(const security::SecureBuffer &master_key,
                    const std::span<const std::byte> record,
                    const std::byte slot) {
  const AlgorithmHandle algorithm = open_sha256_hmac();
  security::SecureBuffer hash_object(object_length(algorithm.get()));
  HashHandle hash;
  require_success(
      ::BCryptCreateHash(algorithm.get(), hash.put(),
                         reinterpret_cast<PUCHAR>(hash_object.data()),
                         checked_ulong(hash_object.size()),
                         mutable_bytes(master_key),
                         checked_ulong(master_key.size()), 0),
      "create envelope HMAC");

  std::array<std::byte, kHmacInputSize> input{};
  auto cursor = input.begin();
  cursor =
      std::ranges::copy(std::as_bytes(std::span(kEnvelopeKeyDomain)), cursor)
          .out;
  cursor =
      std::ranges::copy(record.subspan(kRecordIdOffset, kRecordIdSize), cursor)
          .out;
  *cursor = slot;
  require_success(::BCryptHashData(hash.get(),
                                   reinterpret_cast<PUCHAR>(input.data()),
                                   checked_ulong(input.size()), 0),
                  "hash envelope key domain");

  security::SecureBuffer result(32);
  require_success(::BCryptFinishHash(hash.get(),
                                     reinterpret_cast<PUCHAR>(result.data()),
                                     checked_ulong(result.size()), 0),
                  "finish envelope HMAC");
  return result;
}

[[nodiscard]] AlgorithmHandle open_aes_gcm() {
  AlgorithmHandle algorithm;
  require_success(::BCryptOpenAlgorithmProvider(
                      algorithm.put(), BCRYPT_AES_ALGORITHM, nullptr, 0),
                  "open AES provider");
  require_success(
      ::BCryptSetProperty(algorithm.get(), BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(
                              const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0),
      "set AES-GCM mode");
  return algorithm;
}

class SymmetricKey final {
public:
  SymmetricKey(const BCRYPT_ALG_HANDLE algorithm,
               const security::SecureBuffer &key_bytes)
      : object_(object_length(algorithm)) {
    require_success(::BCryptGenerateSymmetricKey(
                        algorithm, key_.put(),
                        reinterpret_cast<PUCHAR>(object_.data()),
                        checked_ulong(object_.size()), mutable_bytes(key_bytes),
                        checked_ulong(key_bytes.size()), 0),
                    "generate AES-GCM key");
  }

  [[nodiscard]] BCRYPT_KEY_HANDLE get() const noexcept { return key_.get(); }

private:
  security::SecureBuffer object_;
  KeyHandle key_;
};

[[nodiscard]] std::array<std::byte, kAadSize>
envelope_aad(const std::span<const std::byte> record, const std::byte slot) {
  std::array<std::byte, kAadSize> result{};
  std::ranges::copy(record.first(kCardRecordV1HeaderSize), result.begin());
  result.back() = slot;
  return result;
}

void encrypt_envelope(const std::span<std::byte> record,
                      const std::size_t envelope_offset, const std::byte slot,
                      const security::SecureBuffer &root_secret,
                      const security::SecureBuffer &password_utf8) {
  security::SecureBuffer master_key = derive_master_key(password_utf8, record);
  security::SecureBuffer envelope_key =
      derive_envelope_key(master_key, record, slot);
  master_key.clear();

  security::SecureBuffer plaintext(kPlaintextSize);
  std::ranges::copy(root_secret.bytes(), plaintext.bytes().begin());
  std::ranges::copy(kPayloadMarker,
                    plaintext.bytes().begin() + kCardRecordV1RootSecretSize);

  auto envelope = record.subspan(envelope_offset, kCardRecordV1EnvelopeSize);
  fill_random(envelope.first(kNonceSize));
  auto ciphertext = envelope.subspan(kNonceSize, kPlaintextSize);
  auto tag = envelope.last(kTagSize);
  const auto aad = envelope_aad(record, slot);

  const AlgorithmHandle algorithm = open_aes_gcm();
  const SymmetricKey key(algorithm.get(), envelope_key);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = reinterpret_cast<PUCHAR>(envelope.data());
  authentication.cbNonce = checked_ulong(kNonceSize);
  authentication.pbAuthData =
      reinterpret_cast<PUCHAR>(const_cast<std::byte *>(aad.data()));
  authentication.cbAuthData = checked_ulong(aad.size());
  authentication.pbTag = reinterpret_cast<PUCHAR>(tag.data());
  authentication.cbTag = checked_ulong(tag.size());

  ULONG written = 0;
  require_success(
      ::BCryptEncrypt(key.get(), reinterpret_cast<PUCHAR>(plaintext.data()),
                      checked_ulong(plaintext.size()), &authentication, nullptr,
                      0, reinterpret_cast<PUCHAR>(ciphertext.data()),
                      checked_ulong(ciphertext.size()), &written, 0),
      "encrypt card envelope");
  if (written != ciphertext.size()) {
    throw std::runtime_error("AES-GCM produced an invalid ciphertext length");
  }
  plaintext.clear();
  envelope_key.clear();
}

struct EnvelopeResult final {
  bool authenticated{false};
  security::SecureBuffer root_secret{kCardRecordV1RootSecretSize};
};

[[nodiscard]] EnvelopeResult
decrypt_envelope(const std::span<const std::byte> record,
                 const std::size_t envelope_offset, const std::byte slot,
                 const security::SecureBuffer &master_key) {
  security::SecureBuffer envelope_key =
      derive_envelope_key(master_key, record, slot);
  const auto envelope =
      record.subspan(envelope_offset, kCardRecordV1EnvelopeSize);
  const auto ciphertext = envelope.subspan(kNonceSize, kPlaintextSize);
  const auto tag = envelope.last(kTagSize);
  const auto aad = envelope_aad(record, slot);

  const AlgorithmHandle algorithm = open_aes_gcm();
  const SymmetricKey key(algorithm.get(), envelope_key);
  security::SecureBuffer plaintext(kPlaintextSize);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = mutable_bytes(envelope.first(kNonceSize));
  authentication.cbNonce = checked_ulong(kNonceSize);
  authentication.pbAuthData =
      reinterpret_cast<PUCHAR>(const_cast<std::byte *>(aad.data()));
  authentication.cbAuthData = checked_ulong(aad.size());
  authentication.pbTag = mutable_bytes(tag);
  authentication.cbTag = checked_ulong(tag.size());

  ULONG written = 0;
  const NTSTATUS status = ::BCryptDecrypt(
      key.get(), mutable_bytes(ciphertext), checked_ulong(ciphertext.size()),
      &authentication, nullptr, 0, reinterpret_cast<PUCHAR>(plaintext.data()),
      checked_ulong(plaintext.size()), &written, 0);
  envelope_key.clear();
  if (status == kStatusAuthTagMismatch) {
    plaintext.clear();
    return EnvelopeResult{};
  }
  require_success(status, "decrypt card envelope");
  if (written != plaintext.size() ||
      !constant_time_equal(plaintext.bytes().last(kPayloadMarker.size()),
                           kPayloadMarker)) {
    plaintext.clear();
    return EnvelopeResult{};
  }

  EnvelopeResult result;
  result.authenticated = true;
  std::ranges::copy(plaintext.bytes().first(kCardRecordV1RootSecretSize),
                    result.root_secret.bytes().begin());
  plaintext.clear();
  return result;
}

[[nodiscard]] security::SecureBuffer
build_record(const security::SecureBuffer &root_secret,
             const security::SecureBuffer &primary_password_utf8,
             const security::SecureBuffer *const secondary_password_utf8) {
  if (root_secret.size() != kCardRecordV1RootSecretSize) {
    throw std::invalid_argument("card root secret has an invalid size");
  }

  security::SecureBuffer result(kCardRecordV1Size);
  fill_random(result.bytes());
  write_header(result.bytes());
  encrypt_envelope(result.bytes(), kCardRecordV1PrimaryEnvelopeOffset,
                   std::byte{0}, root_secret, primary_password_utf8);
  if (secondary_password_utf8 == nullptr) {
    fill_random(result.bytes().subspan(kCardRecordV1SecondaryEnvelopeOffset,
                                       kCardRecordV1EnvelopeSize));
  } else {
    encrypt_envelope(result.bytes(), kCardRecordV1SecondaryEnvelopeOffset,
                     std::byte{1}, root_secret, *secondary_password_utf8);
  }
  return result;
}

} // namespace

CardCredentialRejected::CardCredentialRejected()
    : std::runtime_error("card credential was rejected") {}

CardRecordState
inspect_card_record(const std::span<const std::byte> record) noexcept {
  if (record.size() == kLegacyP1CardBlobSize) {
    return CardRecordState::legacy_p1_blob;
  }
  if (exact_provisioning_marker(record)) {
    return CardRecordState::provisioned_uninitialized;
  }
  if (valid_v1_header(record)) {
    return CardRecordState::initialized_v1;
  }
  if (record.size() >= kInitializedMagic.size() &&
      std::ranges::equal(record.first(kInitializedMagic.size()),
                         kInitializedMagic)) {
    if (record.size() >= 12 && read_u32(record, 8) != kCardRecordV1Version) {
      return CardRecordState::unsupported_version;
    }
    return CardRecordState::malformed;
  }
  return CardRecordState::malformed;
}

security::SecureBuffer provision_uninitialized_card_record_v1() {
  security::SecureBuffer result(kCardRecordV1Size);
  std::ranges::copy(kProvisioningMagic, result.bytes().begin());
  return result;
}

security::SecureBuffer
initialize_card_record_v1(security::SecureBuffer password_utf8) {
  security::SecureBuffer root_secret(kCardRecordV1RootSecretSize);
  fill_random(root_secret.bytes());
  try {
    security::SecureBuffer result =
        build_record(root_secret, password_utf8, nullptr);
    root_secret.clear();
    password_utf8.clear();
    return result;
  } catch (...) {
    root_secret.clear();
    password_utf8.clear();
    throw;
  }
}

security::SecureBuffer
unlock_card_record_v1(const std::span<const std::byte> record,
                      security::SecureBuffer password_utf8) {
  if (inspect_card_record(record) != CardRecordState::initialized_v1) {
    password_utf8.clear();
    throw std::invalid_argument("card record is not an initialized v1 record");
  }

  try {
    security::SecureBuffer master_key =
        derive_master_key(password_utf8, record);
    EnvelopeResult primary = decrypt_envelope(
        record, kCardRecordV1PrimaryEnvelopeOffset, std::byte{0}, master_key);
    EnvelopeResult secondary = decrypt_envelope(
        record, kCardRecordV1SecondaryEnvelopeOffset, std::byte{1}, master_key);
    master_key.clear();
    password_utf8.clear();

    const unsigned int successful_envelopes =
        static_cast<unsigned int>(primary.authenticated) +
        static_cast<unsigned int>(secondary.authenticated);
    if (successful_envelopes != 1U) {
      primary.root_secret.clear();
      secondary.root_secret.clear();
      if (successful_envelopes == 0U) {
        throw CardCredentialRejected();
      }
      throw std::runtime_error("card record credential is ambiguous");
    }

    const unsigned int primary_mask =
        0U - static_cast<unsigned int>(primary.authenticated);
    security::SecureBuffer result(kCardRecordV1RootSecretSize);
    for (std::size_t index = 0; index < result.size(); ++index) {
      const unsigned int primary_byte =
          std::to_integer<unsigned int>(primary.root_secret.bytes()[index]);
      const unsigned int secondary_byte =
          std::to_integer<unsigned int>(secondary.root_secret.bytes()[index]);
      result.bytes()[index] = static_cast<std::byte>(
          (primary_byte & primary_mask) | (secondary_byte & ~primary_mask));
    }
    primary.root_secret.clear();
    secondary.root_secret.clear();
    return result;
  } catch (...) {
    password_utf8.clear();
    throw;
  }
}

security::SecureBuffer
rewrap_card_record_v1(const std::span<const std::byte> record,
                      security::SecureBuffer existing_password_utf8,
                      security::SecureBuffer primary_password_utf8,
                      security::SecureBuffer secondary_password_utf8) {
  if (constant_time_equal(primary_password_utf8.bytes(),
                          secondary_password_utf8.bytes())) {
    existing_password_utf8.clear();
    primary_password_utf8.clear();
    secondary_password_utf8.clear();
    throw std::invalid_argument(
        "primary and secondary passwords must be different");
  }

  try {
    security::SecureBuffer root_secret =
        unlock_card_record_v1(record, std::move(existing_password_utf8));
    security::SecureBuffer result = build_record(
        root_secret, primary_password_utf8, &secondary_password_utf8);
    root_secret.clear();
    primary_password_utf8.clear();
    secondary_password_utf8.clear();
    return result;
  } catch (...) {
    existing_password_utf8.clear();
    primary_password_utf8.clear();
    secondary_password_utf8.clear();
    throw;
  }
}

} // namespace usb_vault::card
