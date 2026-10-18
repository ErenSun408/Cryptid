#include "usb_vault/backup/transfer_package_v1.h"

#include "backup/transfer_package_v1_detail.h"

#include <Windows.h>
#include <bcrypt.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::backup {

struct TransferTranscriptV1 final {
  crypto_sha512_ctx context{};

  TransferTranscriptV1() noexcept { crypto_sha512_init(&context); }
  ~TransferTranscriptV1() { crypto_wipe(&context, sizeof(context)); }
  TransferTranscriptV1(const TransferTranscriptV1&) = delete;
  TransferTranscriptV1& operator=(const TransferTranscriptV1&) = delete;

  void absorb(std::span<const std::byte> bytes) noexcept {
    crypto_sha512_update(&context,
                         reinterpret_cast<const std::uint8_t*>(bytes.data()),
                         bytes.size());
  }
  void absorb_u64(std::uint64_t value) noexcept {
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
    absorb(bytes);
  }
  [[nodiscard]] std::array<std::byte, 64> finish() noexcept {
    std::array<std::byte, 64> digest{};
    crypto_sha512_final(&context, reinterpret_cast<std::uint8_t*>(digest.data()));
    return digest;
  }
};

namespace {

constexpr std::string_view kSignatureDomain = "usb-vault:transfer-signature:v1";
constexpr std::string_view kSlotKeyDomain = "usb-vault:transfer-slot-key:v1";
constexpr std::string_view kSlotDomain = "usb-vault:transfer-slot:v1";
constexpr std::string_view kChunkKeyDomain = "usb-vault:transfer-chunk-key:v1";
constexpr std::size_t kFileKeySize = 32;
constexpr std::size_t kMacSize = 16;
static_assert(kFileKeySize + kMacSize == kTransferPackageV1SlotSize);

using contacts::ContactIdentity;
using contacts::ContactPublicKey;
using contacts::ContactX25519PublicKey;
using Representative = std::array<std::byte, kTransferPackageV1RepresentativeSize>;
using Prefix = std::array<std::byte, kTransferPackageV1PrefixSize>;

[[nodiscard]] std::uint8_t* u8(std::byte* const bytes) noexcept {
  return reinterpret_cast<std::uint8_t*>(bytes);
}
[[nodiscard]] const std::uint8_t* u8(const std::byte* const bytes) noexcept {
  return reinterpret_cast<const std::uint8_t*>(bytes);
}

void fill_random(std::span<std::byte> bytes) {
  const NTSTATUS status = ::BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            "BCryptGenRandom");
  }
}

// Uniform in [0, bound) by rejection.
[[nodiscard]] std::size_t random_below(const std::size_t bound) {
  const std::uint32_t limit =
      std::numeric_limits<std::uint32_t>::max() -
      std::numeric_limits<std::uint32_t>::max() % static_cast<std::uint32_t>(bound);
  for (;;) {
    std::uint32_t value = 0;
    fill_random(std::as_writable_bytes(std::span(&value, 1)));
    if (value < limit) return value % bound;
  }
}

[[nodiscard]] std::span<const std::byte> text_bytes(std::string_view text) noexcept {
  return std::as_bytes(std::span(text.data(), text.size()));
}

// First half of HMAC-SHA-512(shared, domain || one-time key || recipient key).
[[nodiscard]] security::SecureBuffer slot_key(
    std::span<const std::byte> shared, const ContactX25519PublicKey& one_time,
    const ContactX25519PublicKey& recipient) {
  std::array<std::byte, kSlotKeyDomain.size() + 64> message{};
  std::ranges::copy(text_bytes(kSlotKeyDomain), message.begin());
  std::ranges::copy(one_time, message.begin() + kSlotKeyDomain.size());
  std::ranges::copy(recipient, message.begin() + kSlotKeyDomain.size() + 32);
  security::SecureBuffer mac(64);
  crypto_sha512_hmac(u8(mac.data()), u8(shared.data()), shared.size(),
                     u8(message.data()), message.size());
  security::SecureBuffer key(32);
  std::ranges::copy(mac.bytes().first(32), key.bytes().begin());
  return key;
}

[[nodiscard]] std::array<std::byte, kSlotDomain.size() + 1> slot_ad(
    const std::size_t index) noexcept {
  std::array<std::byte, kSlotDomain.size() + 1> ad{};
  std::ranges::copy(text_bytes(kSlotDomain), ad.begin());
  ad.back() = static_cast<std::byte>(index);
  return ad;
}

// Each slot key is used once, so the all-zero nonce is never repeated.
constexpr std::array<std::uint8_t, 24> kSlotNonce{};

[[nodiscard]] ContactX25519PublicKey one_time_key(
    const Representative& representative) noexcept {
  ContactX25519PublicKey key{};
  crypto_elligator_map(u8(key.data()), u8(representative.data()));
  return key;
}

// The chunk key binds the whole recipient block: a changed block fails at
// the first chunk, so only a holder of the file key can produce a package
// whose content opens but whose signature does not verify.
[[nodiscard]] ExportPackageKeyV1 chunk_key(const security::SecureBuffer& file_key,
                                           std::span<const std::byte> prefix) {
  std::array<std::byte, kChunkKeyDomain.size() + 64> message{};
  std::ranges::copy(text_bytes(kChunkKeyDomain), message.begin());
  crypto_sha512(u8(message.data() + kChunkKeyDomain.size()), u8(prefix.data()),
                prefix.size());
  security::SecureBuffer mac(64);
  crypto_sha512_hmac(u8(mac.data()), u8(file_key.data()), file_key.size(),
                     u8(message.data()), message.size());
  security::SecureBuffer key(32);
  std::ranges::copy(mac.bytes().first(32), key.bytes().begin());
  return ExportPackageKeyV1::transfer(key, prefix);
}

// Merged by X25519 key: two Ed25519 encodings differing only in the sign bit
// share one, and two slots would then repeat a key and nonce.
[[nodiscard]] std::vector<ContactX25519PublicKey> merged_recipients(
    const ContactIdentity& sender, std::span<const ContactPublicKey> recipients) {
  std::vector<ContactX25519PublicKey> everyone{sender.x25519_public_key()};
  for (const ContactPublicKey& recipient : recipients) {
    if (!contacts::contact_public_key_usable(recipient)) {
      throw std::invalid_argument("a transfer recipient key is not usable");
    }
    const ContactX25519PublicKey u = contacts::contact_x25519_public_key(recipient);
    if (std::ranges::find(everyone, u) == everyone.end()) everyone.push_back(u);
  }
  if (everyone.size() > kTransferPackageV1Slots) {
    throw std::invalid_argument("a transfer package has at most 16 recipients");
  }
  return everyone;
}

[[nodiscard]] ExportPackageKeyV1 seal_recipients(
    const ContactIdentity& sender, std::span<const ContactPublicKey> recipients) {
  const std::vector<ContactX25519PublicKey> everyone =
      merged_recipients(sender, recipients);

  Prefix prefix{};
  fill_random(prefix);  // Unused slots stay random.
  security::SecureBuffer seed(32);
  fill_random(seed.bytes());
  security::SecureBuffer one_time_secret(32);
  Representative representative{};
  crypto_elligator_key_pair(u8(representative.data()), u8(one_time_secret.data()),
                            u8(seed.data()));
  std::ranges::copy(representative, prefix.begin());
  const ContactX25519PublicKey one_time = one_time_key(representative);

  security::SecureBuffer file_key(kFileKeySize);
  fill_random(file_key.bytes());

  std::array<std::size_t, kTransferPackageV1Slots> positions{};
  for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = i;
  for (std::size_t i = positions.size() - 1; i > 0; --i) {
    std::swap(positions[i], positions[random_below(i + 1)]);
  }

  security::SecureBuffer shared(32);
  for (std::size_t i = 0; i < everyone.size(); ++i) {
    const ContactX25519PublicKey& recipient = everyone[i];
    crypto_x25519(u8(shared.data()), u8(one_time_secret.data()),
                  u8(recipient.data()));
    const security::SecureBuffer key = slot_key(shared.bytes(), one_time, recipient);
    const std::size_t slot = positions[i];
    const auto ad = slot_ad(slot);
    std::byte* const out = prefix.data() + kTransferPackageV1RepresentativeSize +
                           slot * kTransferPackageV1SlotSize;
    crypto_aead_lock(u8(out), u8(out + kFileKeySize), u8(key.data()),
                     kSlotNonce.data(), u8(ad.data()), ad.size(),
                     u8(file_key.data()), kFileKeySize);
  }
  return chunk_key(file_key, prefix);
}

// Tries every slot, without stopping at the first that opens.
[[nodiscard]] ExportPackageKeyV1 open_recipient(
    const ContactIdentity& recipient, std::span<const std::byte> prefix) {
  if (prefix.size() != kTransferPackageV1PrefixSize) {
    throw std::logic_error("transfer prefix has the wrong size");
  }
  Representative representative{};
  std::ranges::copy(prefix.first(representative.size()), representative.begin());
  const ContactX25519PublicKey one_time = one_time_key(representative);
  const std::optional<security::SecureBuffer> shared = recipient.agree(one_time);
  if (!shared) throw TransferPackageNotAddressed();
  const security::SecureBuffer key =
      slot_key(shared->bytes(), one_time, recipient.x25519_public_key());

  security::SecureBuffer candidate(kFileKeySize);
  security::SecureBuffer file_key(kFileKeySize);
  bool opened = false;
  for (std::size_t slot = 0; slot < kTransferPackageV1Slots; ++slot) {
    const auto ad = slot_ad(slot);
    const std::byte* const in = prefix.data() + kTransferPackageV1RepresentativeSize +
                                slot * kTransferPackageV1SlotSize;
    const bool match =
        crypto_aead_unlock(u8(candidate.data()), u8(in + kFileKeySize),
                           u8(key.data()), kSlotNonce.data(), u8(ad.data()),
                           ad.size(), u8(in), kFileKeySize) == 0;
    if (match && !opened) {
      std::ranges::copy(candidate.bytes(), file_key.bytes().begin());
      opened = true;
    }
  }
  if (!opened) throw TransferPackageNotAddressed();
  return chunk_key(file_key, prefix);
}

// A size no package could have is not addressed to anyone: it may be a
// password package, which the caller tries next.
[[nodiscard]] std::uint64_t transfer_layout_size(const std::uint64_t size) {
  constexpr std::uint64_t kSealedChunk =
      kExportPackageV1ChunkSize + kExportPackageV1TagSize;
  if (size <= kTransferPackageV1PrefixSize + kExportPackageV1TagSize) {
    throw TransferPackageNotAddressed();
  }
  const std::uint64_t body = size - kTransferPackageV1PrefixSize;
  const std::uint64_t chunks = (body + kSealedChunk - 1) / kSealedChunk;
  const std::uint64_t final_sealed = body - (chunks - 1) * kSealedChunk;
  if (final_sealed <= kExportPackageV1TagSize ||
      body - chunks * kExportPackageV1TagSize <
          kExportPackageV1StreamHeaderSize + kTransferPackageV1StreamExtraSize) {
    throw TransferPackageNotAddressed();
  }
  return size;
}

void absorb_header(TransferTranscriptV1& transcript,
                   const ExportPackageV1Header& header) noexcept {
  transcript.absorb_u64(header.manifest_bytes);
  transcript.absorb_u64(header.data_bytes);
  transcript.absorb_u64(header.entry_count);
}

}  // namespace

namespace detail {

ExportPackageKeyV1 open_transfer_package_key(const ContactIdentity& recipient,
                                             std::span<const std::byte> prefix) {
  return open_recipient(recipient, prefix);
}

}  // namespace detail

void validate_transfer_recipients_v1(const ContactIdentity& sender,
                                     std::span<const ContactPublicKey> recipients) {
  static_cast<void>(merged_recipients(sender, recipients));
}

std::uint64_t transfer_package_v1_size(const ExportPackageV1Header& header) {
  return export_package_v1_size(header, kTransferPackageV1PrefixSize,
                                kTransferPackageV1StreamExtraSize);
}

TransferPackageWriterV1::TransferPackageWriterV1(
    const ContactIdentity& sender, std::span<const ContactPublicKey> recipients,
    const ExportPackageV1Header& header, ExportPackageWriterV1::Sink sink)
    : TransferPackageWriterV1(sender, seal_recipients(sender, recipients), header,
                              std::move(sink)) {}

TransferPackageWriterV1::TransferPackageWriterV1(
    const ContactIdentity& sender, ExportPackageKeyV1 key,
    const ExportPackageV1Header& header, ExportPackageWriterV1::Sink sink)
    : sender_(sender),
      transcript_([&] {
        auto transcript = std::make_unique<TransferTranscriptV1>();
        transcript->absorb(text_bytes(kSignatureDomain));
        transcript->absorb(key.prefix());
        absorb_header(*transcript, header);
        transcript->absorb(sender.public_key());
        return transcript;
      }()),
      writer_(std::move(key), header, std::move(sink)),
      remaining_(header.manifest_bytes + header.data_bytes) {
  writer_.write(sender_.public_key());
}

TransferPackageWriterV1::~TransferPackageWriterV1() = default;

void TransferPackageWriterV1::write(std::span<const std::byte> plaintext) {
  if (plaintext.size() > remaining_) {
    throw std::logic_error("more plaintext than the package header declares");
  }
  writer_.write(plaintext);
  transcript_->absorb(plaintext);
  remaining_ -= plaintext.size();
}

void TransferPackageWriterV1::finish() {
  if (finished_) throw std::logic_error("transfer package is already finished");
  if (remaining_ != 0) {
    throw std::logic_error("less plaintext than the package header declares");
  }
  const auto digest = transcript_->finish();
  const contacts::ContactSignature signature = sender_.sign(digest);
  finished_ = true;
  writer_.write(signature);
  writer_.finish();
}

TransferPackageReaderV1::TransferPackageReaderV1(
    const std::uint64_t package_size, ExportPackageReaderV1::Source source,
    const ContactIdentity& recipient)
    : transcript_(std::make_unique<TransferTranscriptV1>()),
      reader_(transfer_layout_size(package_size), std::move(source),
              kTransferPackageV1PrefixSize,
              [this, &recipient](std::span<const std::byte> prefix) {
                ExportPackageKeyV1 key = open_recipient(recipient, prefix);
                transcript_->absorb(text_bytes(kSignatureDomain));
                transcript_->absorb(prefix);
                return key;
              }) {
  absorb_header(*transcript_, reader_.header());
  reader_.read(sender_);
  transcript_->absorb(sender_);
}

TransferPackageReaderV1::~TransferPackageReaderV1() = default;

void TransferPackageReaderV1::read(std::span<std::byte> plaintext) {
  if (plaintext.size() > remaining()) {
    throw std::logic_error("read past the end of the transfer package");
  }
  reader_.read(plaintext);
  transcript_->absorb(plaintext);
}

bool TransferPackageReaderV1::verify_signature() {
  if (verified_ || remaining() != 0) {
    throw std::logic_error("the signature follows all manifest and data bytes");
  }
  verified_ = true;
  contacts::ContactSignature signature{};
  reader_.read(signature);
  const auto digest = transcript_->finish();
  return contacts::verify_contact_signature(sender_, digest, signature);
}

}  // namespace usb_vault::backup
