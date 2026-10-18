#pragma once

#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/contacts/contact_identity_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace usb_vault::backup {

// P5.14 recipient transfer package, format v1. Like a P5.9 package it has no
// header in the clear: it starts with the Elligator2 representative of a
// one-time X25519 key and 16 recipient slots, every byte of which looks
// random, then the same AES-256-GCM chunks under the transfer chunk domain.
// Each used slot wraps the random file key for one recipient; unused slots
// are random, so the number of recipients is not visible. The sender's key
// and signature travel inside the encrypted stream, so only a recipient
// learns who sent the package.
inline constexpr std::size_t kTransferPackageV1Slots = 16;
inline constexpr std::size_t kTransferPackageV1SlotSize = 48;
inline constexpr std::size_t kTransferPackageV1RepresentativeSize = 32;
inline constexpr std::size_t kTransferPackageV1PrefixSize =
    kTransferPackageV1RepresentativeSize +
    kTransferPackageV1Slots * kTransferPackageV1SlotSize;
static_assert(kTransferPackageV1PrefixSize == 800);
static_assert(kTransferPackageV1StreamExtraSize ==
              contacts::kContactPublicKeySize + contacts::kContactSignatureSize);

// No slot opens for this identity: the file is a password package, is meant
// for someone else, or its recipient block is damaged.
class TransferPackageNotAddressed final : public std::runtime_error {
 public:
  TransferPackageNotAddressed()
      : std::runtime_error("the package is not addressed to this card") {}
};

// The content authenticates but the signature does not verify: a recipient
// holding the file key re-encrypted other content under the sender's key. The
// package is refused like a damaged one; nothing of it is published.
class TransferPackageForged final : public ExportPackageRejected {};

[[nodiscard]] std::uint64_t transfer_package_v1_size(
    const ExportPackageV1Header& header);

// Throws std::invalid_argument for a key that is not usable, or for more than
// 16 distinct recipients once the sender and duplicates are merged: the same
// checks TransferPackageWriterV1 makes, for refusing before any work starts.
void validate_transfer_recipients_v1(
    const contacts::ContactIdentity& sender,
    std::span<const contacts::ContactPublicKey> recipients);

// Writes one package for `recipients` plus the sender itself, who can always
// reopen what it sent. Duplicates and the sender's own key are merged; more
// than 16 distinct recipients in all, or a key that is not usable, throw
// std::invalid_argument before the sink sees a byte. The caller supplies
// exactly the manifest and data bytes the header declares; finish() appends
// the signature over the whole package and seals the last chunk. `sender`
// must outlive the writer.
// SHA-512 over everything the signature covers; defined privately.
struct TransferTranscriptV1;

class TransferPackageWriterV1 final {
 public:
  TransferPackageWriterV1(const contacts::ContactIdentity& sender,
                          std::span<const contacts::ContactPublicKey> recipients,
                          const ExportPackageV1Header& header,
                          ExportPackageWriterV1::Sink sink);
  ~TransferPackageWriterV1();
  TransferPackageWriterV1(const TransferPackageWriterV1&) = delete;
  TransferPackageWriterV1& operator=(const TransferPackageWriterV1&) = delete;

  void write(std::span<const std::byte> plaintext);
  void finish();

 private:
  TransferPackageWriterV1(const contacts::ContactIdentity& sender,
                          ExportPackageKeyV1 key,
                          const ExportPackageV1Header& header,
                          ExportPackageWriterV1::Sink sink);

  const contacts::ContactIdentity& sender_;
  std::unique_ptr<TransferTranscriptV1> transcript_;
  ExportPackageWriterV1 writer_;
  std::uint64_t remaining_{};
  bool finished_{false};
};

// Reads a package addressed to `recipient`. Construction reads the recipient
// block and opens the first chunk: TransferPackageNotAddressed when the size
// fits no transfer package or no slot opens, ExportPackageRejected when one
// opens but the stream does not authenticate. read() returns the manifest and
// data; after the last of them, verify_signature() reads the signature and
// reports whether it verifies under sender(). Chunk failures throw
// ExportPackageRejected as in P5.9.
//
// 🔴 Content read before verify_signature() returned true is unattributed:
// a recipient holding the file key can re-encrypt other content under the
// sender's key. Callers stage it and neither publish it nor show sender()
// until the signature has been checked.
class TransferPackageReaderV1 final {
 public:
  TransferPackageReaderV1(std::uint64_t package_size,
                          ExportPackageReaderV1::Source source,
                          const contacts::ContactIdentity& recipient);
  ~TransferPackageReaderV1();
  TransferPackageReaderV1(const TransferPackageReaderV1&) = delete;
  TransferPackageReaderV1& operator=(const TransferPackageReaderV1&) = delete;

  [[nodiscard]] const ExportPackageV1Header& header() const noexcept {
    return reader_.header();
  }
  [[nodiscard]] const contacts::ContactPublicKey& sender() const noexcept {
    return sender_;
  }
  // Manifest and data bytes not read yet.
  [[nodiscard]] std::uint64_t remaining() const noexcept {
    const std::uint64_t left = reader_.remaining();
    return left > contacts::kContactSignatureSize
               ? left - contacts::kContactSignatureSize
               : 0;
  }

  void read(std::span<std::byte> plaintext);
  // Call once remaining() is zero; consumes the signature.
  [[nodiscard]] bool verify_signature();
  // The signature verify_signature() read; zero before it ran.
  [[nodiscard]] const contacts::ContactSignature& signature() const noexcept {
    return signature_;
  }

 private:
  // Filled while reader_ is constructed, so declared before it.
  std::unique_ptr<TransferTranscriptV1> transcript_;
  ExportPackageReaderV1 reader_;
  contacts::ContactPublicKey sender_{};
  contacts::ContactSignature signature_{};
  bool verified_{false};
};

}  // namespace usb_vault::backup
