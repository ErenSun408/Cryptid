#include "usb_vault/backup/export_package_v1.h"

#include "security/argon2id_runtime.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

namespace usb_vault::backup {
namespace {

constexpr std::string_view kMasterDomain = "usb-vault:export-package-master:v1";
constexpr std::string_view kKeyDomain = "usb-vault:export-package-key:v1";
constexpr std::string_view kChunkDomain = "usb-vault:export-package-chunk:v1";
// Longer than kChunkDomain; chunk_aad uses each domain at its own length.
constexpr std::string_view kTransferChunkDomain =
    "usb-vault:transfer-package-chunk:v1";

// Inside the encryption only; a reader reaches it after the first tag has
// already authenticated, so it guards against a format mix-up, not an attacker.
constexpr std::array<std::byte, 8> kStreamMarker{
    std::byte{'U'}, std::byte{'V'}, std::byte{'E'}, std::byte{'X'},
    std::byte{'P'}, std::byte{'K'}, std::byte{'G'}, std::byte{'1'}};
constexpr std::uint32_t kStreamVersion = 1;
constexpr std::uint32_t kTransferStreamVersion = 2;

constexpr std::uint32_t kMemoryKib = 512U * 1024U;
constexpr std::uint32_t kIterations = 2;
constexpr std::uint32_t kParallelism = 1;
constexpr std::size_t kKeySize = 32;
constexpr std::size_t kNonceSize = 12;
// v1 AAD is exactly domain, index and final flag; the transfer domain is
// longer, so the buffer is sized for it and only the used length is passed.
constexpr std::size_t kAadSize = kTransferChunkDomain.size() + 8 + 1;
constexpr std::uint64_t kSealedChunkSize =
    kExportPackageV1ChunkSize + kExportPackageV1TagSize;
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

template <typename Handle, auto Close>
class UniqueHandle final {
 public:
  UniqueHandle() noexcept = default;
  ~UniqueHandle() noexcept { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  [[nodiscard]] Handle get() const noexcept { return handle_; }
  [[nodiscard]] Handle* put() noexcept {
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

[[nodiscard]] PUCHAR mutable_bytes(const std::span<const std::byte> bytes) noexcept {
  return const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data()));
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

[[nodiscard]] security::SecureBuffer derive_package_key(
    const security::SecureBuffer& master_key) {
  AlgorithmHandle algorithm;
  require_success(
      ::BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
                                    nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
      "open SHA-256 HMAC provider");
  security::SecureBuffer hash_object(object_length(algorithm.get()));
  HashHandle hash;
  require_success(
      ::BCryptCreateHash(algorithm.get(), hash.put(),
                         reinterpret_cast<PUCHAR>(hash_object.data()),
                         checked_ulong(hash_object.size()),
                         mutable_bytes(master_key.bytes()),
                         checked_ulong(master_key.size()), 0),
      "create package key HMAC");
  require_success(
      ::BCryptHashData(hash.get(),
                       mutable_bytes(std::as_bytes(std::span(kKeyDomain))),
                       checked_ulong(kKeyDomain.size()), 0),
      "hash package key domain");
  security::SecureBuffer result(kKeySize);
  require_success(::BCryptFinishHash(hash.get(),
                                     reinterpret_cast<PUCHAR>(result.data()),
                                     checked_ulong(result.size()), 0),
                  "finish package key HMAC");
  return result;
}

void write_u32(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
  }
}

void write_u64(const std::span<std::byte> bytes, const std::size_t offset,
               const std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
  }
}

[[nodiscard]] std::uint32_t read_u32(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::span<const std::byte> bytes,
                                     const std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

// Header, manifest, data and any extra stream bytes; std::nullopt when it
// overflows.
[[nodiscard]] std::optional<std::uint64_t> stream_size(
    const ExportPackageV1Header& header,
    const std::size_t extra_stream_bytes = 0) noexcept {
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t fixed = kExportPackageV1StreamHeaderSize + extra_stream_bytes;
  if (header.manifest_bytes > max - fixed) return {};
  const std::uint64_t prefix = fixed + header.manifest_bytes;
  if (header.data_bytes > max - prefix) return {};
  return prefix + header.data_bytes;
}

[[nodiscard]] std::array<std::byte, kExportPackageV1StreamHeaderSize>
encode_header(const ExportPackageV1Header& header, const std::uint32_t version) {
  std::array<std::byte, kExportPackageV1StreamHeaderSize> bytes{};
  std::ranges::copy(kStreamMarker, bytes.begin());
  write_u32(bytes, 8, version);
  write_u32(bytes, 12, 0);
  write_u64(bytes, 16, header.manifest_bytes);
  write_u64(bytes, 24, header.data_bytes);
  write_u64(bytes, 32, header.entry_count);
  return bytes;
}

[[nodiscard]] std::array<std::byte, kNonceSize> chunk_nonce(
    const std::uint64_t index) noexcept {
  std::array<std::byte, kNonceSize> nonce{};
  write_u64(nonce, 0, index);
  return nonce;
}

struct ChunkAad final {
  std::array<std::byte, kAadSize> bytes{};
  std::size_t size{};
};

// Domain, chunk index and final flag, at the domain's own length: password
// packages keep their v1 bytes exactly.
[[nodiscard]] ChunkAad chunk_aad(const std::uint64_t index, const bool final,
                                 const bool transfer) noexcept {
  ChunkAad aad;
  const std::string_view domain = transfer ? kTransferChunkDomain : kChunkDomain;
  std::ranges::copy(std::as_bytes(std::span(domain)), aad.bytes.begin());
  write_u64(aad.bytes, domain.size(), index);
  aad.bytes[domain.size() + 8] = final ? std::byte{1} : std::byte{0};
  aad.size = domain.size() + 9;
  return aad;
}

}  // namespace

ExportPackageRejected::ExportPackageRejected()
    : std::runtime_error(
          "the export password is wrong or the package is damaged") {}

std::uint64_t export_package_v1_size(const ExportPackageV1Header& header) {
  return export_package_v1_size(header, kExportPackageV1SaltSize, 0);
}

std::uint64_t export_package_v1_size(const ExportPackageV1Header& header,
                                     const std::size_t prefix_size,
                                     const std::size_t extra_stream_bytes) {
  const auto stream = stream_size(header, extra_stream_bytes);
  if (!stream) throw std::length_error("export package stream is too large");
  const std::uint64_t chunks =
      (*stream + kExportPackageV1ChunkSize - 1) / kExportPackageV1ChunkSize;
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  if (*stream > max - prefix_size ||
      chunks > (max - prefix_size - *stream) / kExportPackageV1TagSize) {
    throw std::length_error("export package is too large");
  }
  return prefix_size + *stream + chunks * kExportPackageV1TagSize;
}

ExportPackageV1Salt generate_export_package_v1_salt() {
  ExportPackageV1Salt salt{};
  require_success(
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(salt.data()),
                        checked_ulong(salt.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG),
      "BCryptGenRandom");
  return salt;
}

struct ExportPackageKeyV1::State final {
  ExportPackageV1Salt salt{};
  std::vector<std::byte> prefix;
  bool transfer{false};
  AlgorithmHandle algorithm;
  security::SecureBuffer key_object;
  KeyHandle key;
};

ExportPackageKeyV1::ExportPackageKeyV1(State* const state) noexcept
    : state_(state) {}

ExportPackageKeyV1::ExportPackageKeyV1(ExportPackageKeyV1&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

ExportPackageKeyV1& ExportPackageKeyV1::operator=(
    ExportPackageKeyV1&& other) noexcept {
  if (this != &other) {
    delete state_;
    state_ = std::exchange(other.state_, nullptr);
  }
  return *this;
}

ExportPackageKeyV1::~ExportPackageKeyV1() noexcept { delete state_; }

ExportPackageKeyV1 ExportPackageKeyV1::derive(
    const security::SecureBuffer& password_utf8,
    const ExportPackageV1Salt& salt) {
  security::SecureBuffer master = security::detail::derive_argon2id(
      password_utf8, salt, {}, kMasterDomain,
      {.memory_kib = kMemoryKib,
       .iterations = kIterations,
       .parallelism = kParallelism},
      kKeySize);
  security::SecureBuffer key_bytes = derive_package_key(master);
  master.clear();

  auto state = std::make_unique<State>();
  state->salt = salt;
  state->prefix.assign(salt.begin(), salt.end());
  return from_state(std::move(state), key_bytes);
}

ExportPackageKeyV1 ExportPackageKeyV1::transfer(
    const security::SecureBuffer& file_key,
    const std::span<const std::byte> prefix) {
  if (file_key.size() != kKeySize || prefix.empty()) {
    throw std::invalid_argument("invalid transfer package key");
  }
  auto state = std::make_unique<State>();
  state->transfer = true;
  state->prefix.assign(prefix.begin(), prefix.end());
  return from_state(std::move(state), file_key);
}

ExportPackageKeyV1 ExportPackageKeyV1::from_state(
    std::unique_ptr<State> state, const security::SecureBuffer& key_bytes) {
  require_success(::BCryptOpenAlgorithmProvider(state->algorithm.put(),
                                                BCRYPT_AES_ALGORITHM, nullptr,
                                                0),
                  "open AES provider");
  require_success(
      ::BCryptSetProperty(state->algorithm.get(), BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(
                              const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0),
      "set AES-GCM mode");
  state->key_object =
      security::SecureBuffer(object_length(state->algorithm.get()));
  require_success(::BCryptGenerateSymmetricKey(
                      state->algorithm.get(), state->key.put(),
                      reinterpret_cast<PUCHAR>(state->key_object.data()),
                      checked_ulong(state->key_object.size()),
                      mutable_bytes(key_bytes.bytes()),
                      checked_ulong(key_bytes.size()), 0),
                  "generate AES-GCM key");
  return ExportPackageKeyV1(state.release());
}

const ExportPackageV1Salt& ExportPackageKeyV1::salt() const {
  if (state_ == nullptr) throw std::logic_error("export package key was moved");
  if (state_->transfer) throw std::logic_error("a transfer key has no salt");
  return state_->salt;
}

std::span<const std::byte> ExportPackageKeyV1::prefix() const {
  if (state_ == nullptr) throw std::logic_error("export package key was moved");
  return state_->prefix;
}

bool ExportPackageKeyV1::is_transfer() const {
  if (state_ == nullptr) throw std::logic_error("export package key was moved");
  return state_->transfer;
}

void ExportPackageKeyV1::seal_chunk(const std::uint64_t index, const bool final,
                                    const std::span<const std::byte> plaintext,
                                    const std::span<std::byte> sealed) const {
  if (state_ == nullptr) throw std::logic_error("export package key was moved");
  if (plaintext.empty() || plaintext.size() > kExportPackageV1ChunkSize ||
      sealed.size() != plaintext.size() + kExportPackageV1TagSize) {
    throw std::invalid_argument("invalid export package chunk size");
  }
  auto nonce = chunk_nonce(index);
  auto aad = chunk_aad(index, final, state_->transfer);
  const auto tag = sealed.last(kExportPackageV1TagSize);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
  authentication.cbNonce = checked_ulong(nonce.size());
  authentication.pbAuthData = reinterpret_cast<PUCHAR>(aad.bytes.data());
  authentication.cbAuthData = checked_ulong(aad.size);
  authentication.pbTag = reinterpret_cast<PUCHAR>(tag.data());
  authentication.cbTag = checked_ulong(tag.size());
  ULONG written = 0;
  require_success(
      ::BCryptEncrypt(state_->key.get(), mutable_bytes(plaintext),
                      checked_ulong(plaintext.size()), &authentication, nullptr,
                      0, reinterpret_cast<PUCHAR>(sealed.data()),
                      checked_ulong(plaintext.size()), &written, 0),
      "encrypt export package chunk");
  if (written != plaintext.size()) {
    throw std::runtime_error("AES-GCM produced an invalid ciphertext length");
  }
}

bool ExportPackageKeyV1::open_chunk(const std::uint64_t index, const bool final,
                                    const std::span<const std::byte> sealed,
                                    const std::span<std::byte> plaintext) const {
  if (state_ == nullptr) throw std::logic_error("export package key was moved");
  if (plaintext.empty() || plaintext.size() > kExportPackageV1ChunkSize ||
      sealed.size() != plaintext.size() + kExportPackageV1TagSize) {
    throw std::invalid_argument("invalid export package chunk size");
  }
  auto nonce = chunk_nonce(index);
  auto aad = chunk_aad(index, final, state_->transfer);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication{};
  BCRYPT_INIT_AUTH_MODE_INFO(authentication);
  authentication.pbNonce = reinterpret_cast<PUCHAR>(nonce.data());
  authentication.cbNonce = checked_ulong(nonce.size());
  authentication.pbAuthData = reinterpret_cast<PUCHAR>(aad.bytes.data());
  authentication.cbAuthData = checked_ulong(aad.size);
  authentication.pbTag = mutable_bytes(sealed.last(kExportPackageV1TagSize));
  authentication.cbTag = checked_ulong(kExportPackageV1TagSize);
  ULONG written = 0;
  const NTSTATUS status = ::BCryptDecrypt(
      state_->key.get(), mutable_bytes(sealed.first(plaintext.size())),
      checked_ulong(plaintext.size()), &authentication, nullptr, 0,
      reinterpret_cast<PUCHAR>(plaintext.data()),
      checked_ulong(plaintext.size()), &written, 0);
  if (status == kStatusAuthTagMismatch) {
    ::SecureZeroMemory(plaintext.data(), plaintext.size());
    return false;
  }
  require_success(status, "decrypt export package chunk");
  if (written != plaintext.size()) {
    ::SecureZeroMemory(plaintext.data(), plaintext.size());
    throw std::runtime_error("AES-GCM produced an invalid plaintext length");
  }
  return true;
}

WipedBytes& WipedBytes::operator=(WipedBytes&& other) noexcept {
  if (this != &other) {
    wipe();
    bytes_ = std::move(other.bytes_);
  }
  return *this;
}

void WipedBytes::wipe() noexcept {
  if (!bytes_.empty()) ::SecureZeroMemory(bytes_.data(), bytes_.size());
}

ExportPackageWriterV1::ExportPackageWriterV1(ExportPackageKeyV1 key,
                                             const ExportPackageV1Header& header,
                                             Sink sink)
    : key_(std::move(key)),
      sink_(std::move(sink)),
      pending_(kExportPackageV1ChunkSize),
      sealed_(static_cast<std::size_t>(kSealedChunkSize)) {
  const bool transfer = key_.is_transfer();
  const std::size_t extra = transfer ? kTransferPackageV1StreamExtraSize : 0;
  static_cast<void>(export_package_v1_size(header, key_.prefix().size(), extra));
  if (!sink_) throw std::invalid_argument("export package writer needs a sink");
  sink_(key_.prefix());
  const auto encoded =
      encode_header(header, transfer ? kTransferStreamVersion : kStreamVersion);
  std::ranges::copy(encoded, pending_.bytes().begin());
  pending_size_ = encoded.size();
  remaining_ = header.manifest_bytes + header.data_bytes + extra;
}

void ExportPackageWriterV1::seal_pending(const bool final) {
  const auto plaintext = pending_.bytes().first(pending_size_);
  const auto sealed =
      std::span(sealed_).first(pending_size_ + kExportPackageV1TagSize);
  key_.seal_chunk(next_index_, final, plaintext, sealed);
  pending_.wipe();
  pending_size_ = 0;
  ++next_index_;
  sink_(sealed);
}

void ExportPackageWriterV1::require_usable() const {
  if (failed_) throw std::logic_error("export package writer already failed");
  if (finished_) throw std::logic_error("export package is already finished");
}

void ExportPackageWriterV1::write(std::span<const std::byte> plaintext) {
  require_usable();
  if (plaintext.size() > remaining_) {
    throw std::logic_error("more plaintext than the package header declares");
  }
  failed_ = true;
  remaining_ -= plaintext.size();
  while (!plaintext.empty()) {
    // A full chunk is sealed only once more bytes follow it, so the chunk
    // that finish() seals is always the one marked final.
    if (pending_size_ == kExportPackageV1ChunkSize) seal_pending(false);
    const std::size_t take =
        std::min(plaintext.size(), kExportPackageV1ChunkSize - pending_size_);
    std::ranges::copy(plaintext.first(take),
                      pending_.bytes().begin() +
                          static_cast<std::ptrdiff_t>(pending_size_));
    pending_size_ += take;
    plaintext = plaintext.subspan(take);
  }
  failed_ = false;
}

void ExportPackageWriterV1::finish() {
  require_usable();
  if (remaining_ != 0) {
    throw std::logic_error("less plaintext than the package header declares");
  }
  failed_ = true;
  seal_pending(true);
  failed_ = false;
  finished_ = true;
}

ExportPackageReaderV1::ExportPackageReaderV1(const std::uint64_t package_size,
                                             Source source,
                                             const KeyFactory& derive_key)
    : ExportPackageReaderV1(
          package_size, std::move(source), kExportPackageV1SaltSize,
          derive_key ? PrefixKeyFactory([&derive_key](std::span<const std::byte> prefix) {
            ExportPackageV1Salt salt{};
            std::ranges::copy(prefix, salt.begin());
            ExportPackageKeyV1 key = derive_key(salt);
            if (key.is_transfer()) {
              throw std::logic_error("a password package needs a password key");
            }
            return key;
          })
                     : PrefixKeyFactory{}) {}

ExportPackageReaderV1::ExportPackageReaderV1(
    const std::uint64_t package_size, Source source,
    const std::size_t prefix_size, const PrefixKeyFactory& key_for_prefix)
    : source_(std::move(source)),
      chunk_(kExportPackageV1ChunkSize),
      sealed_(static_cast<std::size_t>(kSealedChunkSize)) {
  if (!source_ || !key_for_prefix || prefix_size == 0) {
    throw std::invalid_argument("export package reader needs a source and key");
  }
  // The layout follows from the size alone: full sealed chunks, then one
  // shorter or equal final chunk holding at least one byte.
  if (package_size <= prefix_size + kExportPackageV1TagSize) {
    throw ExportPackageRejected();
  }
  const std::uint64_t body = package_size - prefix_size;
  chunk_count_ = (body + kSealedChunkSize - 1) / kSealedChunkSize;
  const std::uint64_t final_sealed = body - (chunk_count_ - 1) * kSealedChunkSize;
  if (final_sealed <= kExportPackageV1TagSize) throw ExportPackageRejected();
  final_chunk_plaintext_ = final_sealed - kExportPackageV1TagSize;
  const std::uint64_t stream = body - chunk_count_ * kExportPackageV1TagSize;
  if (stream < kExportPackageV1StreamHeaderSize) throw ExportPackageRejected();

  std::vector<std::byte> prefix(prefix_size);
  source_(prefix);
  key_.emplace(key_for_prefix(prefix));
  if (!std::ranges::equal(key_->prefix(), prefix)) {
    throw std::logic_error("export package key belongs to another prefix");
  }
  const bool transfer = key_->is_transfer();
  open_next_chunk();

  const auto header_bytes =
      std::span<const std::byte>(chunk_.bytes()).first(kExportPackageV1StreamHeaderSize);
  const bool marker_matches =
      std::ranges::equal(header_bytes.first(kStreamMarker.size()), kStreamMarker);
  const bool reserved_clear = std::ranges::all_of(
      header_bytes.subspan(40), [](const std::byte b) { return b == std::byte{0}; });
  header_ = ExportPackageV1Header{.manifest_bytes = read_u64(header_bytes, 16),
                                  .data_bytes = read_u64(header_bytes, 24),
                                  .entry_count = read_u64(header_bytes, 32)};
  const auto declared =
      stream_size(header_, transfer ? kTransferPackageV1StreamExtraSize : 0);
  if (!marker_matches ||
      read_u32(header_bytes, 8) !=
          (transfer ? kTransferStreamVersion : kStreamVersion) ||
      read_u32(header_bytes, 12) != 0 || !reserved_clear || !declared ||
      *declared != stream) {
    chunk_.wipe();
    throw ExportPackageRejected();
  }
  chunk_offset_ = kExportPackageV1StreamHeaderSize;
  remaining_ = stream - kExportPackageV1StreamHeaderSize;
}

void ExportPackageReaderV1::open_next_chunk() {
  if (next_index_ >= chunk_count_) {
    throw std::logic_error("no export package chunk is left to open");
  }
  const bool final = next_index_ + 1 == chunk_count_;
  chunk_size_ = final ? static_cast<std::size_t>(final_chunk_plaintext_)
                      : kExportPackageV1ChunkSize;
  const auto sealed =
      std::span(sealed_).first(chunk_size_ + kExportPackageV1TagSize);
  source_(sealed);
  if (!key_->open_chunk(next_index_, final, sealed,
                        chunk_.bytes().first(chunk_size_))) {
    chunk_.wipe();
    if (next_index_ == 0) throw ExportPackageAuthenticationRejected();
    throw ExportPackageRejected();
  }
  ++next_index_;
  chunk_offset_ = 0;
}

void ExportPackageReaderV1::read(std::span<std::byte> plaintext) {
  if (failed_) throw std::logic_error("export package reader already failed");
  if (plaintext.size() > remaining_) {
    throw std::logic_error("read past the end of the export package");
  }
  failed_ = true;
  while (!plaintext.empty()) {
    if (chunk_offset_ == chunk_size_) open_next_chunk();
    const std::size_t take =
        std::min(plaintext.size(), chunk_size_ - chunk_offset_);
    std::ranges::copy(
        std::span<const std::byte>(chunk_.bytes()).subspan(chunk_offset_, take),
        plaintext.begin());
    chunk_offset_ += take;
    remaining_ -= take;
    plaintext = plaintext.subspan(take);
  }
  failed_ = false;
  if (remaining_ == 0) chunk_.wipe();
}

}  // namespace usb_vault::backup
