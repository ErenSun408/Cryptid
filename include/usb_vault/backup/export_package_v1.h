#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace usb_vault::backup {

// P5.9 encrypted export package, format v1. The file is a random salt followed
// by AES-256-GCM chunks and carries no magic, version or length in the clear,
// so without the export password it cannot be told apart from random bytes.
// The key comes from the export password alone: no card secret, login password
// or volume password takes part, and nothing in the package names the volume
// its contents came from.
inline constexpr std::size_t kExportPackageV1SaltSize = 32;
inline constexpr std::size_t kExportPackageV1ChunkSize = 1024U * 1024U;
inline constexpr std::size_t kExportPackageV1TagSize = 16;
inline constexpr std::size_t kExportPackageV1StreamHeaderSize = 64;

using ExportPackageV1Salt = std::array<std::byte, kExportPackageV1SaltSize>;

// What the encrypted stream header declares. The stream is the header, then
// `manifest_bytes` of manifest, then `data_bytes` of file contents.
struct ExportPackageV1Header final {
  std::uint64_t manifest_bytes{};
  std::uint64_t data_bytes{};
  std::uint64_t entry_count{};

  friend bool operator==(const ExportPackageV1Header&,
                         const ExportPackageV1Header&) = default;
};

// Rejects a damaged, truncated, reordered or spliced package. First-chunk
// authentication has a subtype, but still cannot distinguish a wrong password
// from salt or first-chunk damage without changing the package format.
class ExportPackageRejected : public std::runtime_error {
 public:
  ExportPackageRejected();
};

// A first-chunk tag failure cannot distinguish a wrong password from damage
// to the salt or first chunk. Later rejection confirms an invalid package.
class ExportPackageAuthenticationRejected final : public ExportPackageRejected {};

// Exact size of the package a header produces, for space checks before any
// byte is written. Throws std::length_error when the size does not fit.
[[nodiscard]] std::uint64_t export_package_v1_size(
    const ExportPackageV1Header& header);

// Transfer streams carry the 32-byte sender key and the 64-byte signature in
// addition to manifest and data.
inline constexpr std::size_t kTransferPackageV1StreamExtraSize = 32 + 64;

// The same for a package whose prefix is `prefix_size` bytes and whose stream
// carries `extra_stream_bytes` beyond header, manifest and data.
[[nodiscard]] std::uint64_t export_package_v1_size(
    const ExportPackageV1Header& header, std::size_t prefix_size,
    std::size_t extra_stream_bytes);

// A fresh random salt for one export.
[[nodiscard]] ExportPackageV1Salt generate_export_package_v1_salt();

// The AES-256-GCM key of one package. Move-only; the key bytes and the CNG
// key object stay in page-locked memory and are wiped on destruction.
class ExportPackageKeyV1 final {
 public:
  // Argon2id over the export password and salt, then HMAC-SHA-256 into the
  // package key. The password must be non-empty, well-formed UTF-8. The key
  // keeps its salt, so a writer cannot pair it with another salt or reuse one.
  [[nodiscard]] static ExportPackageKeyV1 derive(
      const security::SecureBuffer& password_utf8,
      const ExportPackageV1Salt& salt);

  // P5.14 recipient transfer package key: the same chunk layout under its own
  // chunk AAD domain and stream version 2, whose stream carries the sender
  // key before the manifest and the signature after the data. `prefix` is the
  // package's recipient block, written before the chunks in place of a salt.
  [[nodiscard]] static ExportPackageKeyV1 transfer(
      const security::SecureBuffer& file_key,
      std::span<const std::byte> prefix);

  // Password packages only; throws std::logic_error for a transfer key.
  [[nodiscard]] const ExportPackageV1Salt& salt() const;
  // The bytes the package starts with: the salt, or the recipient block.
  [[nodiscard]] std::span<const std::byte> prefix() const;
  [[nodiscard]] bool is_transfer() const;

  ExportPackageKeyV1(ExportPackageKeyV1&&) noexcept;
  ExportPackageKeyV1& operator=(ExportPackageKeyV1&&) noexcept;
  ExportPackageKeyV1(const ExportPackageKeyV1&) = delete;
  ExportPackageKeyV1& operator=(const ExportPackageKeyV1&) = delete;
  ~ExportPackageKeyV1() noexcept;

  // Seals one chunk of 1..kExportPackageV1ChunkSize bytes into
  // `plaintext.size() + kExportPackageV1TagSize` bytes. The chunk index and
  // whether it is the last chunk are bound into the tag.
  void seal_chunk(std::uint64_t index, bool final,
                  std::span<const std::byte> plaintext,
                  std::span<std::byte> sealed) const;

  // Opens one sealed chunk. Returns false, with `plaintext` wiped, when the
  // tag does not authenticate under this index and final flag.
  [[nodiscard]] bool open_chunk(std::uint64_t index, bool final,
                                std::span<const std::byte> sealed,
                                std::span<std::byte> plaintext) const;

 private:
  struct State;
  explicit ExportPackageKeyV1(State* state) noexcept;
  [[nodiscard]] static ExportPackageKeyV1 from_state(
      std::unique_ptr<State> state, const security::SecureBuffer& key_bytes);
  State* state_{nullptr};
};

// Plaintext staging that is wiped before its memory is released. File
// contents are not key material, but they are what the package protects.
class WipedBytes final {
 public:
  WipedBytes() = default;
  explicit WipedBytes(std::size_t size) : bytes_(size) {}
  WipedBytes(WipedBytes&&) noexcept = default;
  WipedBytes& operator=(WipedBytes&& other) noexcept;
  WipedBytes(const WipedBytes&) = delete;
  WipedBytes& operator=(const WipedBytes&) = delete;
  ~WipedBytes() noexcept { wipe(); }

  [[nodiscard]] std::span<std::byte> bytes() noexcept { return bytes_; }
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }
  void wipe() noexcept;

 private:
  std::vector<std::byte> bytes_;
};

// Writes a package in one pass: the key's salt, then the stream header and
// every plaintext byte supplied through write(), sealed chunk by chunk into
// `sink`. The total supplied must match the header exactly; finish() seals the
// last chunk and refuses a short stream. After any exception the writer
// refuses further calls, since what reached the sink is no longer a prefix it
// can extend.
class ExportPackageWriterV1 final {
 public:
  using Sink = std::function<void(std::span<const std::byte>)>;

  ExportPackageWriterV1(ExportPackageKeyV1 key,
                        const ExportPackageV1Header& header, Sink sink);

  void write(std::span<const std::byte> plaintext);
  void finish();

 private:
  void seal_pending(bool final);
  void require_usable() const;

  ExportPackageKeyV1 key_;
  Sink sink_;
  WipedBytes pending_;
  std::size_t pending_size_{};
  std::vector<std::byte> sealed_;
  std::uint64_t next_index_{};
  std::uint64_t remaining_{};
  bool finished_{false};
  bool failed_{false};
};

// Reads a package in one pass. The chunk layout follows from the package size
// alone; the first chunk is opened while constructing, so a wrong password
// throws ExportPackageRejected before any content is returned. Every later
// failure to authenticate also throws ExportPackageRejected, after which the
// reader refuses further reads. `source` must fill its whole span from the
// package, in order, or throw.
//
// For a transfer key the stream after the header is sender key, manifest,
// data and signature; remaining() and read() cover all of it, and the
// transfer layer splits it.
class ExportPackageReaderV1 final {
 public:
  using Source = std::function<void(std::span<std::byte>)>;
  using KeyFactory =
      std::function<ExportPackageKeyV1(const ExportPackageV1Salt&)>;
  // Receives the package prefix; returns the key whose prefix() equals it.
  using PrefixKeyFactory =
      std::function<ExportPackageKeyV1(std::span<const std::byte>)>;

  ExportPackageReaderV1(std::uint64_t package_size, Source source,
                        const KeyFactory& derive_key);
  ExportPackageReaderV1(std::uint64_t package_size, Source source,
                        std::size_t prefix_size,
                        const PrefixKeyFactory& key_for_prefix);

  [[nodiscard]] const ExportPackageV1Header& header() const noexcept {
    return header_;
  }
  // Stream bytes after the header that read() has not returned yet.
  [[nodiscard]] std::uint64_t remaining() const noexcept { return remaining_; }

  // Returns exactly `plaintext.size()` further bytes; asking past the end is
  // a logic error.
  void read(std::span<std::byte> plaintext);

 private:
  void open_next_chunk();

  std::optional<ExportPackageKeyV1> key_;
  Source source_;
  ExportPackageV1Header header_{};
  std::uint64_t chunk_count_{};
  std::uint64_t final_chunk_plaintext_{};
  std::uint64_t next_index_{};
  WipedBytes chunk_;
  std::size_t chunk_size_{};
  std::size_t chunk_offset_{};
  std::vector<std::byte> sealed_;
  std::uint64_t remaining_{};
  bool failed_{false};
};

}  // namespace usb_vault::backup
