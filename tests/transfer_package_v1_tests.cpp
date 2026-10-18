#include "usb_vault/backup/transfer_package_v1.h"

#include "backup/transfer_package_v1_detail.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using usb_vault::backup::ExportPackageAuthenticationRejected;
using usb_vault::backup::ExportPackageKeyV1;
using usb_vault::backup::ExportPackageReaderV1;
using usb_vault::backup::ExportPackageRejected;
using usb_vault::backup::ExportPackageV1Header;
using usb_vault::backup::ExportPackageWriterV1;
using usb_vault::backup::generate_export_package_v1_salt;
using usb_vault::backup::kTransferPackageV1PrefixSize;
using usb_vault::backup::kTransferPackageV1RepresentativeSize;
using usb_vault::backup::kTransferPackageV1Slots;
using usb_vault::backup::kTransferPackageV1SlotSize;
using usb_vault::backup::transfer_package_v1_size;
using usb_vault::backup::TransferPackageNotAddressed;
using usb_vault::backup::TransferPackageReaderV1;
using usb_vault::backup::TransferPackageWriterV1;
using usb_vault::contacts::ContactIdentity;
using usb_vault::contacts::ContactPublicKey;
using usb_vault::security::SecureBuffer;
using Bytes = std::vector<std::byte>;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] ContactIdentity identity(const unsigned char fill) {
  SecureBuffer secret(32);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    secret.data()[i] = static_cast<std::byte>(fill + i);
  }
  return ContactIdentity::derive(secret);
}

[[nodiscard]] Bytes content(const std::size_t size, const unsigned seed) {
  Bytes bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
  }
  return bytes;
}

struct Package final {
  ExportPackageV1Header header;
  Bytes plaintext;  // Manifest then data, as the import reads them.
  Bytes file;
};

[[nodiscard]] Package write_package(const ContactIdentity& sender,
                                    const std::vector<ContactPublicKey>& to,
                                    const std::size_t manifest,
                                    const std::size_t data) {
  Package package;
  package.header = {.manifest_bytes = manifest, .data_bytes = data, .entry_count = 1};
  package.plaintext = content(manifest + data, 7);
  TransferPackageWriterV1 writer(
      sender, to, package.header, [&](std::span<const std::byte> bytes) {
        package.file.insert(package.file.end(), bytes.begin(), bytes.end());
      });
  // Odd write sizes cross chunk boundaries.
  std::span<const std::byte> rest = package.plaintext;
  while (!rest.empty()) {
    const std::size_t take = std::min<std::size_t>(rest.size(), 70001);
    writer.write(rest.first(take));
    rest = rest.subspan(take);
  }
  writer.finish();
  return package;
}

struct Opened final {
  ContactPublicKey sender{};
  Bytes plaintext;
  bool signature{false};
};

[[nodiscard]] Opened open_package(const Bytes& file, const ContactIdentity& me) {
  std::size_t at = 0;
  TransferPackageReaderV1 reader(
      file.size(),
      [&](std::span<std::byte> out) {
        if (at + out.size() > file.size()) throw std::runtime_error("short");
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(at), out.size(),
                    out.begin());
        at += out.size();
      },
      me);
  Opened opened;
  opened.sender = reader.sender();
  opened.plaintext.resize(static_cast<std::size_t>(reader.remaining()));
  std::span<std::byte> rest = opened.plaintext;
  while (!rest.empty()) {
    const std::size_t take = std::min<std::size_t>(rest.size(), 99991);
    reader.read(rest.first(take));
    rest = rest.subspan(take);
  }
  opened.signature = reader.verify_signature();
  require(at == file.size(), "the reader did not consume the whole package");
  return opened;
}

template <typename E, typename F>
[[nodiscard]] bool throws(F&& f) {
  try {
    f();
  } catch (const E&) {
    return true;
  }
  return false;
}

void test_round_trip_for_recipient_and_sender() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const Package package = write_package(alice, {bob.public_key()}, 300, 2500000);
  require(package.file.size() == transfer_package_v1_size(package.header),
          "the package size does not match its prediction");
  for (const ContactIdentity* reader : {&bob, &alice}) {
    const Opened opened = open_package(package.file, *reader);
    require(opened.sender == alice.public_key(), "the sender was not recovered");
    require(opened.plaintext == package.plaintext, "the content did not round trip");
    require(opened.signature, "a genuine signature did not verify");
  }
}

void test_non_recipients_cannot_open() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const ContactIdentity carol = identity(3);
  const Package package = write_package(alice, {bob.public_key()}, 10, 10);
  require(throws<TransferPackageNotAddressed>(
              [&] { static_cast<void>(open_package(package.file, carol)); }),
          "a non-recipient opened the package");
}

void test_recipient_limits_and_merging() {
  const ContactIdentity alice = identity(1);
  std::vector<ContactPublicKey> others;
  for (unsigned char i = 0; i < 15; ++i) others.push_back(identity(40 + i).public_key());
  others.push_back(alice.public_key());  // The sender again.
  others.push_back(others.front());      // A duplicate.
  const Package full = write_package(alice, others, 5, 5);
  for (unsigned char i = 0; i < 15; ++i) {
    require(open_package(full.file, identity(40 + i)).signature,
            "a recipient of a full package could not open it");
  }
  others.push_back(identity(90).public_key());
  bool touched = false;
  require(throws<std::invalid_argument>([&] {
            TransferPackageWriterV1 writer(alice, others, {.manifest_bytes = 1},
                                           [&](auto) { touched = true; });
          }) && !touched,
          "a seventeenth recipient was accepted");
  ContactPublicKey neutral{};
  neutral[0] = std::byte{1};
  require(throws<std::invalid_argument>([&] {
            const std::vector<ContactPublicKey> bad{neutral};
            TransferPackageWriterV1 writer(alice, bad, {.manifest_bytes = 1},
                                           [&](auto) { touched = true; });
          }) && !touched,
          "an unusable recipient key was accepted");
}

void test_recipient_block_tampering() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const Package package = write_package(alice, {bob.public_key()}, 10, 10);

  Bytes changed = package.file;
  changed[5] ^= std::byte{1};
  require(throws<TransferPackageNotAddressed>(
              [&] { static_cast<void>(open_package(changed, bob)); }),
          "a changed one-time key still opened");

  // The chunk key binds the whole block: Bob's own slot stops the package
  // opening for him, any other slot fails at the first chunk, and so do the
  // representative bits the curve map ignores.
  std::size_t lost = 0;
  for (std::size_t slot = 0; slot < kTransferPackageV1Slots; ++slot) {
    changed = package.file;
    changed[kTransferPackageV1RepresentativeSize +
            slot * kTransferPackageV1SlotSize + 3] ^= std::byte{1};
    try {
      static_cast<void>(open_package(changed, bob));
      throw std::runtime_error("a changed slot still opened");
    } catch (const TransferPackageNotAddressed&) {
      ++lost;
    } catch (const ExportPackageAuthenticationRejected&) {
    }
  }
  require(lost == 1, "Bob's access did not depend on exactly one slot");
  changed = package.file;
  changed[kTransferPackageV1RepresentativeSize - 1] ^= std::byte{0x80};
  require(throws<ExportPackageAuthenticationRejected>(
              [&] { static_cast<void>(open_package(changed, bob)); }),
          "a changed ignored representative bit was not rejected");
}

// Decrypts the whole stream after the header as a recipient: sender key,
// manifest, data and signature.
[[nodiscard]] Bytes recipient_stream(const Bytes& file, const ContactIdentity& me,
                                     ExportPackageV1Header& header) {
  std::size_t at = 0;
  ExportPackageReaderV1 reader(
      file.size(),
      [&](std::span<std::byte> out) {
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(at), out.size(),
                    out.begin());
        at += out.size();
      },
      kTransferPackageV1PrefixSize,
      [&](std::span<const std::byte> prefix) {
        return usb_vault::backup::detail::open_transfer_package_key(me, prefix);
      });
  header = reader.header();
  Bytes stream(static_cast<std::size_t>(reader.remaining()));
  reader.read(stream);
  return stream;
}

// A recipient holding the file key re-encrypts an edited stream under the
// same recipient block, keeping the sender's key and signature.
[[nodiscard]] Bytes reseal(const Bytes& file, const ContactIdentity& me,
                           const ExportPackageV1Header& header, const Bytes& stream) {
  const Bytes prefix(file.begin(), file.begin() + kTransferPackageV1PrefixSize);
  Bytes forged;
  ExportPackageWriterV1 writer(
      usb_vault::backup::detail::open_transfer_package_key(me, prefix), header,
      [&](std::span<const std::byte> bytes) {
        forged.insert(forged.end(), bytes.begin(), bytes.end());
      });
  writer.write(stream);
  writer.finish();
  return forged;
}

void test_recipient_cannot_forge_the_sender() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const ContactIdentity carol = identity(3);
  const Package package =
      write_package(alice, {bob.public_key(), carol.public_key()}, 100, 300);
  ExportPackageV1Header header;
  const Bytes stream = recipient_stream(package.file, bob, header);
  constexpr std::size_t kSender = 32;

  // Resealing unchanged content keeps a valid signature.
  require(open_package(reseal(package.file, bob, header, stream), carol).signature,
          "an unchanged reseal lost its signature");

  for (const std::size_t at : {kSender + 5, kSender + 100 + 7, kSender + 399}) {
    Bytes edited = stream;
    edited[at] ^= std::byte{1};
    const Opened opened = open_package(reseal(package.file, bob, header, edited), carol);
    require(opened.sender == alice.public_key() && !opened.signature,
            "edited manifest or data kept the sender's signature");
  }
  ExportPackageV1Header recounted = header;
  ++recounted.entry_count;
  require(!open_package(reseal(package.file, bob, recounted, stream), carol).signature,
          "a changed entry count kept the sender's signature");
  Bytes impersonated = stream;
  std::ranges::copy(carol.public_key(), impersonated.begin());
  require(!open_package(reseal(package.file, bob, header, impersonated), carol).signature,
          "a swapped sender key verified with the original signature");
}

void test_sign_bit_twins_share_one_slot() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  ContactPublicKey twin = bob.public_key();
  twin[31] ^= std::byte{0x80};
  if (!usb_vault::contacts::contact_public_key_usable(twin)) return;
  const Package package = write_package(alice, {bob.public_key(), twin}, 10, 10);
  const Bytes prefix(package.file.begin(),
                     package.file.begin() + kTransferPackageV1PrefixSize);
  for (std::size_t a = 0; a < kTransferPackageV1Slots; ++a) {
    for (std::size_t b = a + 1; b < kTransferPackageV1Slots; ++b) {
      require(!std::equal(prefix.begin() + 32 + a * kTransferPackageV1SlotSize,
                          prefix.begin() + 32 + a * kTransferPackageV1SlotSize + 32,
                          prefix.begin() + 32 + b * kTransferPackageV1SlotSize),
              "two slots carry the same ciphertext");
    }
  }
}

void test_stream_tampering_is_rejected() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const Package package = write_package(alice, {bob.public_key()}, 100, 1500000);
  for (const std::size_t at :
       {kTransferPackageV1PrefixSize + 1, kTransferPackageV1PrefixSize + 1200000,
        package.file.size() - 1}) {
    Bytes changed = package.file;
    changed[at] ^= std::byte{1};
    require(throws<ExportPackageRejected>(
                [&] { static_cast<void>(open_package(changed, bob)); }),
            "a changed chunk was not rejected");
  }
  Bytes truncated(package.file.begin(), package.file.end() - 1);
  require(throws<ExportPackageRejected>(
              [&] { static_cast<void>(open_package(truncated, bob)); }),
          "a truncated package was not rejected");
}

void test_formats_do_not_cross() {
  const ContactIdentity bob = identity(2);
  // A password package is not addressed to anyone.
  SecureBuffer password(12);
  std::ranges::fill(password.bytes(), std::byte{'p'});
  Bytes password_file;
  ExportPackageV1Header header{.manifest_bytes = 900, .data_bytes = 900, .entry_count = 1};
  {
    ExportPackageWriterV1 writer(
        ExportPackageKeyV1::derive(password, generate_export_package_v1_salt()),
        header, [&](std::span<const std::byte> bytes) {
          password_file.insert(password_file.end(), bytes.begin(), bytes.end());
        });
    const Bytes body = content(1800, 3);
    writer.write(body);
    writer.finish();
  }
  require(throws<TransferPackageNotAddressed>(
              [&] { static_cast<void>(open_package(password_file, bob)); }),
          "a password package opened as a transfer package");
  for (const std::size_t size : {std::size_t{100}, std::size_t{816}, std::size_t{817},
                                 std::size_t{800 + 1048576 + 16 + 10}}) {
    const Bytes small(size, std::byte{0x42});
    require(throws<TransferPackageNotAddressed>(
                [&] { static_cast<void>(open_package(small, bob)); }),
            "a size no transfer package has was not reported as not addressed");
  }

  // A transfer package does not open with a password key.
  const Package transfer = write_package(identity(1), {bob.public_key()}, 900, 900);
  std::size_t at = 0;
  require(throws<ExportPackageRejected>([&] {
            ExportPackageReaderV1 reader(
                transfer.file.size(),
                [&](std::span<std::byte> out) {
                  std::copy_n(transfer.file.begin() + static_cast<std::ptrdiff_t>(at),
                              out.size(), out.begin());
                  at += out.size();
                },
                [&](const auto& salt) { return ExportPackageKeyV1::derive(password, salt); });
          }),
          "a transfer package opened with a password key");
}

void test_packages_are_fresh_and_misuse_is_refused() {
  const ContactIdentity alice = identity(1);
  const ContactIdentity bob = identity(2);
  const Package first = write_package(alice, {bob.public_key()}, 10, 10);
  const Package second = write_package(alice, {bob.public_key()}, 10, 10);
  std::size_t equal = 0;
  for (std::size_t i = 0; i < first.file.size(); ++i) equal += first.file[i] == second.file[i];
  require(equal < first.file.size() / 16, "two packages of the same input share bytes");

  Bytes sink;
  const std::vector<ContactPublicKey> to_bob{bob.public_key()};
  TransferPackageWriterV1 writer(alice, to_bob,
                                 {.manifest_bytes = 4, .entry_count = 1},
                                 [&](std::span<const std::byte> bytes) {
                                   sink.insert(sink.end(), bytes.begin(), bytes.end());
                                 });
  const Bytes five = content(5, 1);
  require(throws<std::logic_error>([&] { writer.write(five); }),
          "more plaintext than declared was accepted");
  require(throws<std::logic_error>([&] { writer.finish(); }),
          "a short package was finished");
  {
    Bytes out;
    TransferPackageWriterV1 done(alice, to_bob, {.manifest_bytes = 1, .entry_count = 1},
                                 [&](std::span<const std::byte> bytes) {
                                   out.insert(out.end(), bytes.begin(), bytes.end());
                                 });
    const Bytes one = content(1, 2);
    done.write(one);
    done.finish();
    const std::size_t size = out.size();
    require(throws<std::logic_error>([&] { done.finish(); }) && out.size() == size,
            "a finished package was finished again");
  }

  std::size_t at = 0;
  TransferPackageReaderV1 reader(
      first.file.size(),
      [&](std::span<std::byte> out) {
        std::copy_n(first.file.begin() + static_cast<std::ptrdiff_t>(at), out.size(),
                    out.begin());
        at += out.size();
      },
      bob);
  require(throws<std::logic_error>([&] { static_cast<void>(reader.verify_signature()); }),
          "the signature was checked before the content was read");
}

}  // namespace

int main() {
  try {
    test_round_trip_for_recipient_and_sender();
    test_non_recipients_cannot_open();
    test_recipient_limits_and_merging();
    test_recipient_block_tampering();
    test_recipient_cannot_forge_the_sender();
    test_sign_bit_twins_share_one_slot();
    test_stream_tampering_is_rejected();
    test_formats_do_not_cross();
    test_packages_are_fresh_and_misuse_is_refused();
  } catch (const std::exception& error) {
    std::cerr << "transfer_package_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "transfer_package_v1_tests passed\n";
  return 0;
}
