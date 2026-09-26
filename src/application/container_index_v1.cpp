#include "usb_vault/application/container_index_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace usb_vault::application {
namespace {

constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kPlaintextSize =
    kContainerIndexV1SlotSize - kNonceSize - kTagSize;

// Sealed layout, little-endian throughout:
//
//   [0, 4)     format version
//   [4, 6)     path length, in UTF-16 code units
//   [6, 524)   path, padded to its full capacity with random bytes
//   [524, 528) v2: display order; v1: random
//   [528, 532) v6: creation sequence; earlier versions: random
//   [532, 740) reserved, random
//
// The reserved tail is what lets a later version carry more than a path
// without changing the size of a slot, and so without changing the size of the
// file. That matters: the size of the file is the one thing about it an
// observer can measure, so it has to be decided once, here, and never revised.
constexpr std::size_t kVersionOffset = 0;
constexpr std::size_t kPathLengthOffset = 4;
constexpr std::size_t kPathOffset = 6;
constexpr std::size_t kOrderOffset = 524;
constexpr std::uint32_t kOrderedEntryVersion = 2;
constexpr std::uint32_t kPartitionEntryVersion = 3;
// v6 reads its layout from field 24 rather than from the version, always
// carries the conversion state, and adds the creation sequence. It is what
// every partition record written from now on uses.
constexpr std::uint32_t kSequencedPartitionVersion = 6;
constexpr std::size_t kSequenceOffset = 528;
constexpr std::size_t kConversionOffset = 48;
constexpr std::size_t kPartitionIdOffset = 52;
// v3: device ID [4,20), flags [20,24), layout [24,28), sector [28,32),
// offset [32,40), length [40,48), order [524,528). Remaining bytes are random.
// UINT32_MAX is an unordered partition; no field outside the GCM payload.

constexpr std::size_t kPathCapacity =
    kContainerIndexV1PathMaximumLength * sizeof(wchar_t);

constexpr NTSTATUS kStatusAuthTagMismatch = static_cast<NTSTATUS>(0xC000A002UL);

static_assert(sizeof(wchar_t) == 2, "a slot path is stored as UTF-16");
static_assert(kContainerIndexV1KeySize == 32,
              "AES-256-GCM takes a 32-byte key");
static_assert(kContainerIndexV1SlotSize > kNonceSize + kTagSize);
static_assert(kPathOffset + kPathCapacity <= kPlaintextSize,
              "a maximum-length path has to fit in one slot");
static_assert(kContainerIndexV1PathMaximumLength <=
                  std::numeric_limits<std::uint16_t>::max(),
              "a path length has to fit the sealed length field");

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

void fill_random(const std::span<std::byte> destination) {
  require_success(
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(destination.data()),
                        checked_ulong(destination.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG),
      "BCryptGenRandom");
}

void write_u16(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint16_t value) noexcept {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

[[nodiscard]] std::uint16_t read_u16(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<unsigned int>(bytes[offset]) |
      (std::to_integer<unsigned int>(bytes[offset + 1]) << 8U));
}

void write_u32(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  write_u16(bytes, offset, static_cast<std::uint16_t>(value & 0xffffU));
  write_u16(bytes, offset + 2,
            static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(read_u16(bytes, offset)) |
         (static_cast<std::uint32_t>(read_u16(bytes, offset + 2)) << 16U);
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
               const std::span<const std::byte> key_bytes)
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

void require_key(const std::span<const std::byte> key) {
  if (key.size() != kContainerIndexV1KeySize) {
    throw std::invalid_argument("container index key has an invalid size");
  }
}

void require_slot_size(const std::span<const std::byte> slot) {
  if (slot.size() != kContainerIndexV1SlotSize) {
    throw std::invalid_argument("container index slot has an invalid size");
  }
}

void require_whole_file(const std::size_t size) {
  if (size != kContainerIndexV1FileSize) {
    throw std::invalid_argument(
        "the container index buffer is not a whole file");
  }
}

void require_slot_index(const std::size_t index) {
  if (index >= kContainerIndexV1SlotCount) {
    throw std::invalid_argument("the container index slot is past the end");
  }
}

} // namespace

bool valid_container_index_partition(const ContainerIndexPartition& partition) noexcept {
  return volume::valid_partition_identity(partition);
}

static security::SecureBuffer seal_index_entry(
    const std::span<const std::byte> key, const std::wstring_view path,
    const std::optional<std::uint32_t> order,
    const ContainerIndexPartition* const partition,
    PartitionConversionState conversion = PartitionConversionState::normal,
    std::uint32_t sequence = 0) {
  require_key(key);
  if (static_cast<std::uint32_t>(conversion)>2 || (!partition && conversion!=PartitionConversionState::normal))
    throw std::invalid_argument("invalid partition conversion state");
  if (order && *order >= kContainerIndexV1SlotCount) {
    throw std::invalid_argument("container index order is out of range");
  }
  if (partition && !valid_container_index_partition(*partition))
    throw std::invalid_argument("invalid partition index identity");
  if (sequence && !partition)
    throw std::invalid_argument("only a partition record carries a sequence");
  if (!partition && path.empty()) {
    throw std::invalid_argument("container index path is empty");
  }
  if (path.size() > kContainerIndexV1PathMaximumLength) {
    throw std::invalid_argument("container index path is too long");
  }
  if (path.find(L'\0') != std::wstring_view::npos) {
    throw std::invalid_argument("container index path contains a null");
  }

  // The whole plaintext is random before any field is written, so the padding
  // after a short path is indistinguishable from the padding after a long one
  // even to somebody who has recovered the plaintext by other means.
  security::SecureBuffer plaintext(kPlaintextSize);
  fill_random(plaintext.bytes());
  if (partition) {
    const bool gpt = partition->layout == 2;
    write_u32(plaintext.bytes(), kVersionOffset, sequence ? kSequencedPartitionVersion
        : (gpt ? 5 : (conversion == PartitionConversionState::normal
                          ? kPartitionEntryVersion : 4)));
    if (sequence) {
      write_u32(plaintext.bytes(), kConversionOffset, static_cast<std::uint32_t>(conversion));
      write_u32(plaintext.bytes(), kSequenceOffset, sequence);
    } else if (gpt || conversion != PartitionConversionState::normal) {
      write_u32(plaintext.bytes(), kConversionOffset, static_cast<std::uint32_t>(conversion));
    }
    if (gpt) for (std::size_t i = 0; i < partition->partition_id.size(); ++i)
      plaintext.bytes()[kPartitionIdOffset + i] = static_cast<std::byte>(partition->partition_id[i]);
    for (std::size_t i = 0; i < partition->device_id.size(); ++i)
      plaintext.bytes()[4 + i] = static_cast<std::byte>(partition->device_id[i]);
    write_u32(plaintext.bytes(), 20, partition->device_id_flags);
    write_u32(plaintext.bytes(), 24, partition->layout);
    write_u32(plaintext.bytes(), 28, partition->logical_sector_bytes);
    write_u32(plaintext.bytes(), 32, static_cast<std::uint32_t>(partition->offset_bytes));
    write_u32(plaintext.bytes(), 36, static_cast<std::uint32_t>(partition->offset_bytes >> 32));
    write_u32(plaintext.bytes(), 40, static_cast<std::uint32_t>(partition->length_bytes));
    write_u32(plaintext.bytes(), 44, static_cast<std::uint32_t>(partition->length_bytes >> 32));
    write_u32(plaintext.bytes(), kOrderOffset, order.value_or(UINT32_MAX));
  } else {
    write_u32(plaintext.bytes(), kVersionOffset,
              order ? kOrderedEntryVersion : kContainerIndexEntryV1Version);
    if (order) write_u32(plaintext.bytes(), kOrderOffset, *order);
    write_u16(plaintext.bytes(), kPathLengthOffset,
              static_cast<std::uint16_t>(path.size()));
    for (std::size_t index = 0; index < path.size(); ++index) {
      write_u16(plaintext.bytes(), kPathOffset + (index * sizeof(wchar_t)),
                static_cast<std::uint16_t>(path[index]));
    }
  }

  security::SecureBuffer slot(kContainerIndexV1SlotSize);
  const auto slot_bytes = slot.bytes();
  const auto nonce = slot_bytes.first(kNonceSize);
  const auto ciphertext = slot_bytes.subspan(kNonceSize, kPlaintextSize);
  const auto tag = slot_bytes.last(kTagSize);
  fill_random(nonce);

  // A slot is sealed with no associated data, on purpose. Binding a slot to
  // its position would make position meaningful, and position has to stay
  // meaningless: the store must be free to write an entry into whichever slot
  // is spare and to shuffle the file, so that neither the order of the slots
  // nor the choice among them says when an entry was made or how many exist.
  const AlgorithmHandle algorithm = open_aes_gcm();
  const SymmetricKey symmetric_key(algorithm.get(), key);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
  authentication.cbNonce = checked_ulong(nonce.size());
  authentication.pbTag = reinterpret_cast<PUCHAR>(tag.data());
  authentication.cbTag = checked_ulong(tag.size());

  ULONG written = 0;
  require_success(
      ::BCryptEncrypt(symmetric_key.get(),
                      reinterpret_cast<PUCHAR>(plaintext.data()),
                      checked_ulong(plaintext.size()), &authentication, nullptr,
                      0, reinterpret_cast<PUCHAR>(ciphertext.data()),
                      checked_ulong(ciphertext.size()), &written, 0),
      "seal container index entry");
  plaintext.clear();
  if (written != ciphertext.size()) {
    throw std::runtime_error("AES-GCM produced an invalid ciphertext length");
  }
  return slot;
}

security::SecureBuffer seal_container_index_entry_v1(
    const std::span<const std::byte> key, const std::wstring_view path,
    const std::optional<std::uint32_t> order) {
  return seal_index_entry(key, path, order, nullptr);
}

security::SecureBuffer seal_container_index_partition(
    const std::span<const std::byte> key, const ContainerIndexPartition& partition,
    const std::optional<std::uint32_t> order, const PartitionConversionState conversion,
    const std::uint32_t sequence) {
  return seal_index_entry(key, {}, order, &partition, conversion, sequence);
}

std::optional<ContainerIndexEntry>
try_open_container_index_entry(const std::span<const std::byte> key,
                                  const std::span<const std::byte> slot) {
  require_key(key);
  require_slot_size(slot);

  const auto nonce = slot.first(kNonceSize);
  const auto ciphertext = slot.subspan(kNonceSize, kPlaintextSize);
  const auto tag = slot.last(kTagSize);

  const AlgorithmHandle algorithm = open_aes_gcm();
  const SymmetricKey symmetric_key(algorithm.get(), key);
  security::SecureBuffer plaintext(kPlaintextSize);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = mutable_bytes(nonce);
  authentication.cbNonce = checked_ulong(nonce.size());
  authentication.pbTag = mutable_bytes(tag);
  authentication.cbTag = checked_ulong(tag.size());

  ULONG written = 0;
  const NTSTATUS status =
      ::BCryptDecrypt(symmetric_key.get(), mutable_bytes(ciphertext),
                      checked_ulong(ciphertext.size()), &authentication,
                      nullptr, 0, reinterpret_cast<PUCHAR>(plaintext.data()),
                      checked_ulong(plaintext.size()), &written, 0);
  if (status == kStatusAuthTagMismatch) {
    plaintext.clear();
    return std::nullopt;
  }
  require_success(status, "open container index entry");
  if (written != plaintext.size()) {
    plaintext.clear();
    throw std::runtime_error("AES-GCM produced an invalid plaintext length");
  }

  // Everything past this point answers the same way as a failed tag. A slot
  // this key authenticates but that does not read as a supported entry is worth
  // exactly as much to the caller as random filler, and saying more about
  // which it was would hand out the distinction this format exists to deny.
  const auto contents = plaintext.bytes();
  const std::uint32_t version = read_u32(contents, kVersionOffset);
  const bool sequenced = version == kSequencedPartitionVersion;
  if (sequenced || version == kPartitionEntryVersion || version == 4 || version == 5) {
    const auto state = version >= 4 ? read_u32(contents, kConversionOffset) : 0u;
    if (state > 2 || (version == 4 && state == 0)) return std::nullopt;
    // A v6 record must carry a sequence; zero there is not a legacy record but
    // a malformed one, and reads as filler like any other unsupported slot.
    const auto sequence = sequenced ? read_u32(contents, kSequenceOffset) : 0u;
    if (sequenced && sequence == 0) return std::nullopt;
    ContainerIndexPartition partition;
    for (std::size_t i = 0; i < partition.device_id.size(); ++i)
      partition.device_id[i] = std::to_integer<std::uint8_t>(contents[4 + i]);
    partition.device_id_flags = read_u32(contents, 20);
    partition.layout = read_u32(contents, 24);
    // Before v6 the version implied the layout. v6 states it, so it is read and
    // then checked against what a valid identity allows.
    if (!sequenced && partition.layout != (version == 5 ? 2U : 1U)) return std::nullopt;
    if (sequenced ? partition.layout == 2 : version == 5)
      for (std::size_t i = 0; i < partition.partition_id.size(); ++i)
        partition.partition_id[i] = std::to_integer<std::uint8_t>(contents[kPartitionIdOffset + i]);
    partition.logical_sector_bytes = read_u32(contents, 28);
    partition.offset_bytes = read_u32(contents, 32) |
        (static_cast<std::uint64_t>(read_u32(contents, 36)) << 32);
    partition.length_bytes = read_u32(contents, 40) |
        (static_cast<std::uint64_t>(read_u32(contents, 44)) << 32);
    const auto rank = read_u32(contents, kOrderOffset);
    if (!valid_container_index_partition(partition) ||
        (rank != UINT32_MAX && rank >= kContainerIndexV1SlotCount)) return std::nullopt;
    return ContainerIndexEntry{{}, rank == UINT32_MAX ? std::nullopt :
        std::optional<std::uint32_t>(rank), partition,
        static_cast<PartitionConversionState>(state), sequence};
  }
  const std::size_t length = read_u16(contents, kPathLengthOffset);
  if ((version != kContainerIndexEntryV1Version && version != kOrderedEntryVersion) || length == 0 ||
      length > kContainerIndexV1PathMaximumLength) {
    plaintext.clear();
    return std::nullopt;
  }

  std::optional<std::uint32_t> order;
  if (version == kOrderedEntryVersion) {
    order = read_u32(contents, kOrderOffset);
    if (*order >= kContainerIndexV1SlotCount) return std::nullopt;
  }
  std::wstring path(length, L'\0');
  for (std::size_t index = 0; index < length; ++index) {
    path[index] = static_cast<wchar_t>(
        read_u16(contents, kPathOffset + (index * sizeof(wchar_t))));
  }
  plaintext.clear();
  if (path.find(L'\0') != std::wstring::npos) {
    return std::nullopt;
  }
  return ContainerIndexEntry{std::move(path), order, std::nullopt};
}

std::optional<std::wstring>
try_open_container_index_entry_v1(const std::span<const std::byte> key,
                                  const std::span<const std::byte> slot) {
  auto opened = try_open_container_index_entry(key, slot);
  if (!opened || opened->partition) return std::nullopt;
  return std::move(opened->path);
}

void fill_container_index_slot_v1(const std::span<std::byte> slot) {
  require_slot_size(slot);
  fill_random(slot);
}

std::span<const std::byte>
container_index_v1_slot(const std::span<const std::byte> file,
                        const std::size_t index) {
  require_whole_file(file.size());
  require_slot_index(index);
  return file.subspan(index * kContainerIndexV1SlotSize,
                      kContainerIndexV1SlotSize);
}

std::span<std::byte>
container_index_v1_mutable_slot(const std::span<std::byte> file,
                                const std::size_t index) {
  require_whole_file(file.size());
  require_slot_index(index);
  return file.subspan(index * kContainerIndexV1SlotSize,
                      kContainerIndexV1SlotSize);
}

} // namespace usb_vault::application
