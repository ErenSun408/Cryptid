#include "usb_vault/contacts/contact_store_v1.h"

#include "card/card_record_journal_v1.h"
#include "contacts/contact_store_v1_format.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace usb_vault::contacts {

static_assert(kContactStoreV1RegionOffset ==
                  card::kCardRecordJournalV1ReservedSize,
              "the contact list must start where the card journal ends");

namespace detail {
namespace {

constexpr std::string_view kStoreKeyDomain = "usb-vault:contact-store:v1";
constexpr std::string_view kDescriptorDomain =
    "usb-vault:contact-store-descriptor:v1";
constexpr std::string_view kBodyDomain = "usb-vault:contact-store-body:v1";
constexpr NTSTATUS kStatusAuthTagMismatch =
    static_cast<NTSTATUS>(0xC000A002UL);

[[noreturn]] void throw_ntstatus(const NTSTATUS status,
                                 const char* const operation) {
  throw std::system_error(static_cast<int>(status), std::system_category(),
                          operation);
}

void require_success(const NTSTATUS status, const char* const operation) {
  if (!BCRYPT_SUCCESS(status)) throw_ntstatus(status, operation);
}

[[nodiscard]] PUCHAR mutable_bytes(std::span<const std::byte> bytes) noexcept {
  return reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()));
}

[[nodiscard]] ULONG checked_ulong(const std::size_t value) {
  if (value > std::numeric_limits<ULONG>::max()) {
    throw std::length_error("CNG buffer is too large");
  }
  return static_cast<ULONG>(value);
}

class Algorithm final {
 public:
  Algorithm(const wchar_t* const id, const ULONG flags,
            const char* const operation) {
    require_success(::BCryptOpenAlgorithmProvider(&handle_, id, nullptr, flags),
                    operation);
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

// Key objects live in page-locked memory; the handle is destroyed first.
class GcmKey final {
 public:
  explicit GcmKey(const security::SecureBuffer& key)
      : algorithm_(BCRYPT_AES_ALGORITHM, 0, "open AES provider"),
        object_(object_length(algorithm_.get())) {
    require_success(
        ::BCryptSetProperty(algorithm_.get(), BCRYPT_CHAINING_MODE,
                            reinterpret_cast<PUCHAR>(
                                const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                            static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
                            0),
        "set AES-GCM mode");
    require_success(::BCryptGenerateSymmetricKey(
                        algorithm_.get(), &handle_,
                        reinterpret_cast<PUCHAR>(object_.data()),
                        checked_ulong(object_.size()),
                        mutable_bytes(key.bytes()),
                        checked_ulong(key.size()), 0),
                    "generate AES-GCM key");
  }
  ~GcmKey() { static_cast<void>(::BCryptDestroyKey(handle_)); }
  GcmKey(const GcmKey&) = delete;
  GcmKey& operator=(const GcmKey&) = delete;

  void seal(std::span<const std::byte> nonce, std::span<const std::byte> aad,
            std::span<const std::byte> plain, std::span<std::byte> cipher,
            std::span<std::byte> tag) const {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mutable_bytes(nonce);
    info.cbNonce = checked_ulong(nonce.size());
    info.pbAuthData = mutable_bytes(aad);
    info.cbAuthData = checked_ulong(aad.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = checked_ulong(tag.size());
    ULONG written = 0;
    require_success(
        ::BCryptEncrypt(handle_, mutable_bytes(plain),
                        checked_ulong(plain.size()), &info, nullptr, 0,
                        reinterpret_cast<PUCHAR>(cipher.data()),
                        checked_ulong(cipher.size()), &written, 0),
        "encrypt contact store");
    if (written != cipher.size()) {
      throw std::runtime_error("AES-GCM produced an invalid ciphertext length");
    }
  }

  // False when the tag does not authenticate; plain is then cleared.
  [[nodiscard]] bool open(std::span<const std::byte> nonce,
                          std::span<const std::byte> aad,
                          std::span<const std::byte> cipher,
                          std::span<const std::byte> tag,
                          std::span<std::byte> plain) const {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mutable_bytes(nonce);
    info.cbNonce = checked_ulong(nonce.size());
    info.pbAuthData = mutable_bytes(aad);
    info.cbAuthData = checked_ulong(aad.size());
    info.pbTag = mutable_bytes(tag);
    info.cbTag = checked_ulong(tag.size());
    ULONG written = 0;
    const NTSTATUS status = ::BCryptDecrypt(
        handle_, mutable_bytes(cipher), checked_ulong(cipher.size()), &info,
        nullptr, 0, reinterpret_cast<PUCHAR>(plain.data()),
        checked_ulong(plain.size()), &written, 0);
    if (status == kStatusAuthTagMismatch) {
      SecureZeroMemory(plain.data(), plain.size());
      return false;
    }
    require_success(status, "decrypt contact store");
    if (written != plain.size()) {
      SecureZeroMemory(plain.data(), plain.size());
      return false;
    }
    return true;
  }

 private:
  // Declaration order: the key handle is destroyed before its object.
  Algorithm algorithm_;
  security::SecureBuffer object_;
  BCRYPT_KEY_HANDLE handle_{};
};

void fill_random(std::span<std::byte> bytes) {
  require_success(::BCryptGenRandom(nullptr,
                                    reinterpret_cast<PUCHAR>(bytes.data()),
                                    checked_ulong(bytes.size()),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                  "generate random bytes");
}

void put_u16(std::span<std::byte> out, const std::uint16_t value) noexcept {
  out[0] = static_cast<std::byte>(value & 0xFF);
  out[1] = static_cast<std::byte>(value >> 8);
}

void put_u32(std::span<std::byte> out, const std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
  }
}

void put_u64(std::span<std::byte> out, const std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
  }
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::byte> in) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(in[0]) |
                                    (std::to_integer<unsigned>(in[1]) << 8));
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> in) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(in[i]))
             << (8 * i);
  }
  return value;
}

[[nodiscard]] std::span<const std::byte> text_bytes(
    const std::string_view text) noexcept {
  return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] std::vector<std::byte> descriptor_aad(
    const std::size_t copy_offset) {
  std::vector<std::byte> aad(kDescriptorDomain.size() + 8);
  std::ranges::copy(text_bytes(kDescriptorDomain), aad.begin());
  const auto tail = std::span(aad).subspan(kDescriptorDomain.size());
  put_u32(tail.first(4), static_cast<std::uint32_t>(copy_offset));
  put_u32(tail.subspan(4), static_cast<std::uint32_t>(kContactStoreV1RegionSize));
  return aad;
}

[[nodiscard]] std::vector<std::byte> body_aad(
    std::span<const std::byte> descriptor) {
  std::vector<std::byte> aad(kBodyDomain.size() + descriptor.size());
  std::ranges::copy(text_bytes(kBodyDomain), aad.begin());
  std::ranges::copy(descriptor, aad.begin() + kBodyDomain.size());
  return aad;
}

[[nodiscard]] std::size_t copy_index_of(const std::size_t copy_offset) {
  if (copy_offset == kContactStoreV1CopyOffsets[0]) return 0;
  if (copy_offset == kContactStoreV1CopyOffsets[1]) return 1;
  throw std::invalid_argument("not a contact store copy offset");
}

[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) noexcept {
  return std::ranges::all_of(bytes,
                             [](const std::byte b) { return b == std::byte{}; });
}

[[nodiscard]] std::optional<std::vector<ContactEntry>> decode_entries(
    std::span<const std::byte> body, const std::size_t count) {
  if (count > kContactStoreV1MaxContacts) return std::nullopt;
  std::vector<ContactEntry> entries;
  entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto entry = body.subspan(i * kEntrySize, kEntrySize);
    ContactEntry decoded;
    std::ranges::copy(entry.first(kContactPublicKeySize),
                      decoded.public_key.begin());
    const std::size_t length =
        std::to_integer<std::size_t>(entry[kContactPublicKeySize]);
    if (length == 0 || length > kContactRemarkMaxBytes) return std::nullopt;
    const auto remark = entry.subspan(kContactPublicKeySize + 1);
    if (!all_zero(remark.subspan(length))) return std::nullopt;
    decoded.remark.assign(reinterpret_cast<const char*>(remark.data()), length);
    entries.push_back(std::move(decoded));
  }
  if (!all_zero(body.subspan(count * kEntrySize))) return std::nullopt;
  for (const ContactEntry& entry : entries) {
    const auto normalized = normalize_contact_remark(entry.remark);
    if (!normalized.remark || *normalized.remark != entry.remark) {
      return std::nullopt;
    }
  }
  if (validate_contact_entries(entries) != ContactEntriesError::none) {
    return std::nullopt;
  }
  return entries;
}

}  // namespace

security::SecureBuffer derive_store_key(
    const security::SecureBuffer& card_secret) {
  if (card_secret.size() != 32) {
    throw std::invalid_argument("contact store requires a 32-byte secret");
  }
  const Algorithm hmac(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG,
                       "open SHA-256 HMAC provider");
  security::SecureBuffer object(object_length(hmac.get()));
  // Allocated before the hash so nothing can throw while the handle is open.
  security::SecureBuffer key(32);
  BCRYPT_HASH_HANDLE hash{};
  require_success(::BCryptCreateHash(hmac.get(), &hash,
                                     reinterpret_cast<PUCHAR>(object.data()),
                                     checked_ulong(object.size()),
                                     mutable_bytes(card_secret.bytes()),
                                     checked_ulong(card_secret.size()), 0),
                  "create contact store HMAC");
  NTSTATUS status =
      ::BCryptHashData(hash, mutable_bytes(text_bytes(kStoreKeyDomain)),
                       checked_ulong(kStoreKeyDomain.size()), 0);
  if (BCRYPT_SUCCESS(status)) {
    status = ::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(key.data()),
                                checked_ulong(key.size()), 0);
  }
  static_cast<void>(::BCryptDestroyHash(hash));
  require_success(status, "derive contact store key");
  return key;
}

security::SecureBuffer encode_body(std::span<const ContactEntry> entries) {
  if (entries.size() > kContactStoreV1MaxContacts) {
    throw std::invalid_argument("too many contacts");
  }
  security::SecureBuffer body(kBodyPlainSize);
  std::ranges::fill(body.bytes(), std::byte{});
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const ContactEntry& entry = entries[i];
    if (entry.remark.empty() || entry.remark.size() > kContactRemarkMaxBytes) {
      throw std::invalid_argument("contact remark has an invalid length");
    }
    const auto out = body.bytes().subspan(i * kEntrySize, kEntrySize);
    std::ranges::copy(entry.public_key, out.begin());
    out[kContactPublicKeySize] = static_cast<std::byte>(entry.remark.size());
    std::ranges::copy(text_bytes(entry.remark),
                      out.begin() + kContactPublicKeySize + 1);
  }
  return body;
}

Copy seal_copy(const security::SecureBuffer& key, const std::size_t copy_offset,
               const CopyHeader& header,
               std::span<const std::byte> body_plain) {
  static_cast<void>(copy_index_of(copy_offset));
  if (body_plain.size() != kBodyPlainSize) {
    throw std::invalid_argument("contact store body has the wrong size");
  }
  std::array<std::byte, kDescriptorPlainSize> plain{};
  const auto out = std::span(plain);
  put_u16(out.subspan(0, 2), header.format_version);
  out[2] = static_cast<std::byte>(header.layout);
  out[3] = static_cast<std::byte>(header.copy_index);
  put_u64(out.subspan(4, 8), header.generation);
  put_u16(out.subspan(12, 2), header.entry_count);
  put_u16(out.subspan(14, 2), header.body_plain_size);
  out[kDescriptorReservedOffset] = static_cast<std::byte>(header.reserved_probe);

  Copy copy{};
  const auto whole = std::span(copy);
  const auto descriptor = whole.first(kDescriptorSize);
  const auto body = whole.subspan(kDescriptorSize);
  fill_random(descriptor.first(kNonceSize));
  fill_random(body.first(kNonceSize));

  const GcmKey gcm(key);
  gcm.seal(descriptor.first(kNonceSize), descriptor_aad(copy_offset), plain,
           descriptor.subspan(kNonceSize, kDescriptorPlainSize),
           descriptor.last(kTagSize));
  gcm.seal(body.first(kNonceSize), body_aad(descriptor), body_plain,
           body.subspan(kNonceSize, kBodyPlainSize), body.last(kTagSize));
  return copy;
}

OpenedCopy open_copy(const security::SecureBuffer& key,
                     const std::size_t copy_offset,
                     std::span<const std::byte> copy) {
  const std::size_t index = copy_index_of(copy_offset);
  if (copy.size() != kContactStoreV1CopySize) {
    throw std::invalid_argument("contact store copy has the wrong size");
  }
  const auto descriptor = copy.first(kDescriptorSize);
  const auto body = copy.subspan(kDescriptorSize);
  const GcmKey gcm(key);

  OpenedCopy opened;
  std::array<std::byte, kDescriptorPlainSize> plain{};
  if (!gcm.open(descriptor.first(kNonceSize), descriptor_aad(copy_offset),
                descriptor.subspan(kNonceSize, kDescriptorPlainSize),
                descriptor.last(kTagSize), plain)) {
    return opened;
  }
  const auto in = std::span<const std::byte>(plain);
  opened.generation = get_u64(in.subspan(4, 8));
  if (get_u16(in.subspan(0, 2)) != kFormatVersion ||
      std::to_integer<std::uint8_t>(in[2]) != kLayoutTwoCopies) {
    opened.status = CopyStatus::unsupported;
    return opened;
  }
  opened.status = CopyStatus::damaged;
  const std::size_t count = get_u16(in.subspan(12, 2));
  if (std::to_integer<std::size_t>(in[3]) != index ||
      get_u16(in.subspan(14, 2)) != kBodyPlainSize ||
      !all_zero(in.subspan(kDescriptorReservedOffset))) {
    return opened;
  }

  security::SecureBuffer body_plain(kBodyPlainSize);
  if (!gcm.open(body.first(kNonceSize), body_aad(descriptor),
                body.subspan(kNonceSize, kBodyPlainSize), body.last(kTagSize),
                body_plain.bytes())) {
    return opened;
  }
  auto entries = decode_entries(body_plain.bytes(), count);
  if (!entries) return opened;
  opened.entries = std::move(*entries);
  opened.status = CopyStatus::intact;
  return opened;
}

Resolution resolve_region(const security::SecureBuffer& key,
                          std::span<const std::byte> region) {
  if (region.size() != kContactStoreV1RegionSize) {
    throw std::invalid_argument("contact store region has the wrong size");
  }
  OpenedCopy copies[2];
  for (std::size_t i = 0; i < 2; ++i) {
    copies[i] = open_copy(key, kContactStoreV1CopyOffsets[i],
                          region.subspan(i * kContactStoreV1CopySize,
                                         kContactStoreV1CopySize));
  }

  Resolution resolution;
  bool any_damaged = false;
  for (const OpenedCopy& copy : copies) {
    if (copy.status == CopyStatus::unsupported) {
      resolution.state = ContactStoreState::unsupported_version;
      return resolution;
    }
    if (copy.status != CopyStatus::invalid) {
      resolution.highest_generation =
          std::max(resolution.highest_generation, copy.generation);
    }
    any_damaged = any_damaged || copy.status == CopyStatus::damaged;
  }

  const bool intact[2] = {copies[0].status == CopyStatus::intact,
                          copies[1].status == CopyStatus::intact};
  if (!intact[0] && !intact[1]) {
    // Write over whichever copy carries the lower authenticated generation.
    resolution.next_copy = copies[0].status != CopyStatus::invalid &&
                                   copies[1].generation < copies[0].generation
                               ? 1
                               : 0;
    resolution.state =
        any_damaged ? ContactStoreState::damaged : ContactStoreState::empty;
    return resolution;
  }

  std::size_t newest = intact[0] ? 0 : 1;
  if (intact[0] && intact[1]) {
    if (copies[0].generation == copies[1].generation) {
      if (copies[0].entries != copies[1].entries) {
        resolution.next_copy = 0;
        resolution.state = ContactStoreState::inconsistent;
        return resolution;
      }
    } else if (copies[1].generation > copies[0].generation) {
      newest = 1;
    }
  }
  resolution.entries = std::move(copies[newest].entries);
  resolution.selected_generation = copies[newest].generation;
  resolution.next_copy = 1 - newest;
  resolution.state = ContactStoreState::loaded;
  return resolution;
}

}  // namespace detail

namespace {

// Decodes one UTF-8 scalar; returns 0 bytes consumed on any malformation.
[[nodiscard]] std::size_t decode_scalar(const std::string_view text,
                                        const std::size_t at,
                                        char32_t& scalar) noexcept {
  const auto byte = [&](const std::size_t i) {
    return static_cast<unsigned char>(text[i]);
  };
  const unsigned lead = byte(at);
  std::size_t length = 0;
  char32_t minimum = 0;
  if (lead < 0x80) {
    scalar = lead;
    return 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    scalar = lead & 0x1F;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    scalar = lead & 0x0F;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    scalar = lead & 0x07;
    minimum = 0x10000;
  } else {
    return 0;
  }
  if (at + length > text.size()) return 0;
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned next = byte(at + i);
    if ((next & 0xC0) != 0x80) return 0;
    scalar = (scalar << 6) | (next & 0x3F);
  }
  if (scalar < minimum || scalar > 0x10FFFF ||
      (scalar >= 0xD800 && scalar <= 0xDFFF)) {
    return 0;
  }
  return length;
}

// Controls, invisible format and bidirectional overrides would let a remark
// impersonate another in the list; noncharacters are never meaningful text.
[[nodiscard]] bool allowed_in_remark(const char32_t c) noexcept {
  if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return false;
  // Fillers and blanks that render as nothing.
  if (c == 0x034F || c == 0x115F || c == 0x1160 || c == 0x17B4 ||
      c == 0x17B5 || c == 0x2800 || c == 0x3164 || c == 0xFFA0) {
    return false;
  }
  if ((c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF)) {
    return false;
  }
  if (c == 0x00AD || c == 0x061C || c == 0x180E) return false;
  if (c >= 0x200B && c <= 0x200F) return false;
  if (c >= 0x2028 && c <= 0x202E) return false;
  if (c >= 0x2060 && c <= 0x206F) return false;
  if (c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB)) return false;
  if (c >= 0xFDD0 && c <= 0xFDEF) return false;
  if ((c & 0xFFFE) == 0xFFFE) return false;
  if (c >= 0xE0000 && c <= 0xE007F) return false;
  return true;
}

// Unicode White_Space.
[[nodiscard]] bool is_white_space(const char32_t c) noexcept {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Input must already be well-formed UTF-8.
[[nodiscard]] std::string_view trim_remark(std::string_view text) noexcept {
  std::size_t begin = 0;
  while (begin < text.size()) {
    char32_t scalar = 0;
    const std::size_t length = decode_scalar(text, begin, scalar);
    if (!is_white_space(scalar)) break;
    begin += length;
  }
  std::size_t end = begin;
  std::size_t content_end = begin;
  while (end < text.size()) {
    char32_t scalar = 0;
    const std::size_t length = decode_scalar(text, end, scalar);
    end += length;
    if (!is_white_space(scalar)) content_end = end;
  }
  return text.substr(begin, content_end - begin);
}

}  // namespace

ContactRemarkNormalization normalize_contact_remark(
    const std::string_view remark) {
  ContactRemarkNormalization result;
  for (std::size_t at = 0; at < remark.size();) {
    char32_t scalar = 0;
    const std::size_t length = decode_scalar(remark, at, scalar);
    if (length == 0) return result;
    at += length;
  }
  const std::string_view trimmed = trim_remark(remark);
  if (trimmed.empty()) {
    result.error = ContactRemarkError::empty;
    return result;
  }
  for (std::size_t at = 0; at < trimmed.size();) {
    char32_t scalar = 0;
    const std::size_t length = decode_scalar(trimmed, at, scalar);
    if (length == 0 || !allowed_in_remark(scalar)) return result;
    at += length;
  }
  if (trimmed.size() > kContactRemarkMaxBytes) {
    result.error = ContactRemarkError::too_long;
    return result;
  }
  result.remark = std::string(trimmed);
  result.error = ContactRemarkError::none;
  return result;
}

ContactEntriesError validate_contact_entries(
    std::span<const ContactEntry> entries) {
  if (entries.size() > kContactStoreV1MaxContacts) {
    return ContactEntriesError::too_many;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto normalized = normalize_contact_remark(entries[i].remark);
    if (!normalized.remark || *normalized.remark != entries[i].remark) {
      return ContactEntriesError::invalid_remark;
    }
    if (!contact_public_key_usable(entries[i].public_key)) {
      return ContactEntriesError::unusable_key;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (entries[j].public_key == entries[i].public_key) {
        return ContactEntriesError::duplicate_key;
      }
    }
  }
  return ContactEntriesError::none;
}

ContactStoreV1::ContactStoreV1(IContactStoreStorage& storage,
                               const security::SecureBuffer& card_secret)
    : storage_(storage), key_(detail::derive_store_key(card_secret)) {}

ContactStoreState ContactStoreV1::load() {
  state_ = ContactStoreState::unread;
  entries_.clear();
  region_.clear();
  highest_generation_ = 0;
  next_copy_ = 0;

  if (storage_.capacity() < kContactStoreV1RegionEnd) {
    state_ = ContactStoreState::unavailable;
    return state_;
  }
  const security::SecureBuffer region =
      storage_.read(kContactStoreV1RegionOffset, kContactStoreV1RegionSize);
  if (region.size() != kContactStoreV1RegionSize) {
    throw std::runtime_error("contact store read returned the wrong length");
  }
  detail::Resolution resolution = detail::resolve_region(key_, region.bytes());
  region_.assign(region.bytes().begin(), region.bytes().end());
  entries_ = std::move(resolution.entries);
  highest_generation_ = resolution.highest_generation;
  next_copy_ = resolution.next_copy;
  state_ = resolution.state;
  return state_;
}

void ContactStoreV1::save(std::span<const ContactEntry> entries) {
  if (state_ != ContactStoreState::empty &&
      state_ != ContactStoreState::loaded) {
    throw std::logic_error("contact store is not writable in this state");
  }
  if (validate_contact_entries(entries) != ContactEntriesError::none) {
    throw std::invalid_argument("contact list is not valid");
  }
  write_copy(entries);
}

void ContactStoreV1::discard_and_clear() {
  if (state_ != ContactStoreState::damaged &&
      state_ != ContactStoreState::inconsistent) {
    throw std::logic_error("contact store has nothing to discard");
  }
  write_copy({});
}

void ContactStoreV1::reload_after_failure() noexcept {
  try {
    static_cast<void>(load());
  } catch (...) {
    state_ = ContactStoreState::unread;
    entries_.clear();
    region_.clear();
  }
}

void ContactStoreV1::write_copy(std::span<const ContactEntry> entries) {
  if (highest_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("contact store generation is exhausted");
  }
  // Copied first: entries may alias entries_, and nothing below may fail
  // after the card has been written for want of memory.
  std::vector<ContactEntry> next(entries.begin(), entries.end());
  const std::uint64_t generation = highest_generation_ + 1;
  const std::size_t index = next_copy_;
  const std::size_t offset = kContactStoreV1CopyOffsets[index];

  detail::CopyHeader header;
  header.copy_index = static_cast<std::uint8_t>(index);
  header.generation = generation;
  header.entry_count = static_cast<std::uint16_t>(next.size());
  const security::SecureBuffer body = detail::encode_body(next);
  const detail::Copy copy = detail::seal_copy(key_, offset, header, body.bytes());
  std::vector<std::byte> written;

  try {
    const security::SecureBuffer current =
        storage_.read(kContactStoreV1RegionOffset, kContactStoreV1RegionSize);
    if (!std::ranges::equal(current.bytes(), region_)) {
      throw ContactStoreChangedError();
    }

    storage_.write(offset, copy);

    const security::SecureBuffer after =
        storage_.read(kContactStoreV1RegionOffset, kContactStoreV1RegionSize);
    const auto copy_bytes = after.bytes().subspan(
        offset - kContactStoreV1RegionOffset, kContactStoreV1CopySize);
    const detail::Resolution resolution =
        detail::resolve_region(key_, after.bytes());
    if (!std::ranges::equal(copy_bytes, copy) ||
        resolution.state != ContactStoreState::loaded ||
        resolution.selected_generation != generation ||
        resolution.entries != next) {
      throw std::runtime_error("contact store copy is not selected on read-back");
    }
    written.assign(after.bytes().begin(), after.bytes().end());
  } catch (...) {
    reload_after_failure();
    throw;
  }

  region_ = std::move(written);
  entries_ = std::move(next);
  highest_generation_ = generation;
  next_copy_ = 1 - index;
  state_ = ContactStoreState::loaded;
}

}  // namespace usb_vault::contacts
