#include "usb_vault/contacts/contact_identity_v1.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using usb_vault::contacts::ContactIdentity;
using usb_vault::contacts::ContactKeyFileError;
using usb_vault::contacts::ContactPublicKey;
using usb_vault::contacts::ContactSignature;
using usb_vault::contacts::ContactX25519PublicKey;
using usb_vault::contacts::contact_public_key_usable;
using usb_vault::contacts::contact_x25519_public_key;
using usb_vault::contacts::decode_contact_key_file;
using usb_vault::contacts::encode_contact_key_file;
using usb_vault::contacts::format_contact_fingerprint;
using usb_vault::contacts::verify_contact_signature;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secret_of(const unsigned char fill) {
  SecureBuffer secret(32);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    secret.data()[i] = static_cast<std::byte>(fill + i);
  }
  return secret;
}

template <std::size_t N>
[[nodiscard]] std::array<std::byte, N> from_hex(const char* const hex) {
  std::array<std::byte, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = static_cast<std::byte>(std::stoi(std::string(hex + 2 * i, 2),
                                              nullptr, 16));
  }
  return out;
}

[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const std::byte b : bytes) {
    text += kDigits[std::to_integer<unsigned>(b) >> 4];
    text += kDigits[std::to_integer<unsigned>(b) & 0xF];
  }
  return text;
}

[[nodiscard]] std::span<const std::byte> bytes_of(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

void test_derivation_is_deterministic_and_bound_to_secret() {
  const ContactIdentity first = ContactIdentity::derive(secret_of(1));
  const ContactIdentity again = ContactIdentity::derive(secret_of(1));
  const ContactIdentity other = ContactIdentity::derive(secret_of(2));
  require(first.public_key() == again.public_key(),
          "the same card secret derived a different identity");
  require(first.public_key() != other.public_key(),
          "different card secrets derived the same identity");
  require(first.x25519_public_key() ==
              contact_x25519_public_key(first.public_key()),
          "the X25519 key does not match the converted Ed25519 key");

  // Frozen output: changing the derivation would silently replace every
  // card's identity and orphan the keys shared with its contacts. Checked
  // against an independent RFC 8032 implementation of HMAC-SHA-256 + Ed25519.
  require(to_hex(first.public_key()) ==
              "0b1c29f63b9fecd76a85a95d68b546095d13b243d566e051c0ec52175b10ab68",
          "the frozen identity derivation changed");

  bool rejected = false;
  try {
    static_cast<void>(ContactIdentity::derive(SecureBuffer(31)));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "a 31-byte card secret was accepted");
}

void test_standard_ed25519_vector() {
  // RFC 8032 section 7.1, test 1: proves SHA-512 Ed25519, not BLAKE2b EdDSA.
  const auto key = from_hex<32>(
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  const auto signature = from_hex<64>(
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  require(verify_contact_signature(key, {}, signature),
          "the RFC 8032 test vector did not verify");
}

void test_sign_and_verify() {
  const ContactIdentity identity = ContactIdentity::derive(secret_of(3));
  const ContactIdentity other = ContactIdentity::derive(secret_of(4));
  const std::string message = "transfer package digest";
  const ContactSignature signature = identity.sign(bytes_of(message));
  require(verify_contact_signature(identity.public_key(), bytes_of(message),
                                   signature),
          "a signature did not verify");
  require(!verify_contact_signature(other.public_key(), bytes_of(message),
                                    signature),
          "a signature verified under another key");
  std::string changed = message;
  changed[0] = 'T';
  require(!verify_contact_signature(identity.public_key(), bytes_of(changed),
                                    signature),
          "a signature verified for another message");
  ContactSignature bent = signature;
  bent[10] ^= std::byte{1};
  require(!verify_contact_signature(identity.public_key(), bytes_of(message),
                                    bent),
          "a modified signature verified");
}

void test_key_agreement() {
  const ContactIdentity alice = ContactIdentity::derive(secret_of(5));
  const ContactIdentity bob = ContactIdentity::derive(secret_of(6));
  const auto ab =
      alice.agree(contact_x25519_public_key(bob.public_key()));
  const auto ba =
      bob.agree(contact_x25519_public_key(alice.public_key()));
  require(ab && ba, "agreement with a valid key failed");
  require(std::equal(ab->bytes().begin(), ab->bytes().end(),
                     ba->bytes().begin(), ba->bytes().end()),
          "the two sides agreed on different values");

  require(!alice.agree(ContactX25519PublicKey{}),
          "agreement with a low-order key was not refused");
}

void test_low_order_keys_are_unusable() {
  ContactPublicKey identity_point{};  // y = 1, the neutral element
  identity_point[0] = std::byte{1};
  require(!contact_public_key_usable(identity_point),
          "the neutral point was usable");
  require(!contact_public_key_usable(ContactPublicKey{}),
          "an all-zero key was usable");
  require(contact_public_key_usable(
              ContactIdentity::derive(secret_of(7)).public_key()),
          "a derived key was unusable");
}

void test_fingerprint_format() {
  const ContactIdentity a = ContactIdentity::derive(secret_of(8));
  const ContactIdentity b = ContactIdentity::derive(secret_of(9));
  const std::string text = format_contact_fingerprint(a.public_key());
  require(text.size() == 19, "the fingerprint is not 16 symbols in 4 groups");
  for (const std::size_t at : {4U, 9U, 14U}) {
    require(text[at] == '-', "fingerprint groups are not hyphen separated");
  }
  require(text == format_contact_fingerprint(a.public_key()),
          "the fingerprint is not deterministic");
  require(text != format_contact_fingerprint(b.public_key()),
          "two keys share a fingerprint");
}

void test_key_file_round_trip_and_tolerance() {
  const ContactPublicKey key =
      ContactIdentity::derive(secret_of(10)).public_key();
  const std::string file = encode_contact_key_file(key);
  require(file.starts_with("USB-VAULT-CONTACT-KEY v1\r\n"),
          "the key file header is wrong");

  const auto decoded = decode_contact_key_file(file);
  require(decoded.error == ContactKeyFileError::none && decoded.public_key &&
              *decoded.public_key == key,
          "the key file did not round trip");

  std::string relaxed = "\xEF\xBB\xBFUSB-VAULT-CONTACT-KEY v1\n  ";
  for (const char c : file.substr(file.find('\n') + 1)) {
    if (c == '\r' || c == '-') continue;
    relaxed += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  const auto tolerant = decode_contact_key_file(relaxed);
  require(tolerant.public_key && *tolerant.public_key == key,
          "BOM, LF, lower case or missing hyphens were not tolerated");
}

void test_key_file_rejections() {
  const ContactPublicKey key =
      ContactIdentity::derive(secret_of(11)).public_key();
  const std::string file = encode_contact_key_file(key);
  const std::size_t line = file.find('\n') + 1;

  std::string future = file;
  future.replace(0, 24, "USB-VAULT-CONTACT-KEY v2");
  require(decode_contact_key_file(future).error ==
              ContactKeyFileError::unsupported_version,
          "another version was not reported as unsupported");

  require(decode_contact_key_file("hello\r\nworld").error ==
              ContactKeyFileError::malformed,
          "a foreign file was not malformed");
  require(!decode_contact_key_file(file + "extra\r\n").public_key,
          "trailing content was accepted");
  require(!decode_contact_key_file(file.substr(0, file.size() - 4))
               .public_key,
          "a truncated key was accepted");
  require(!decode_contact_key_file(std::string(5000, 'A')).public_key,
          "an oversized file was accepted");

  // Every symbol change is caught by the checksum or the padding rule.
  for (std::size_t i = line; i < file.size(); ++i) {
    if (file[i] == '-' || file[i] == '\r' || file[i] == '\n') continue;
    std::string changed = file;
    changed[i] = changed[i] == '2' ? '3' : '2';
    require(!decode_contact_key_file(changed).public_key,
            "a modified key line still decoded");
  }
}

void test_low_order_key_file_is_rejected() {
  ContactPublicKey neutral{};
  neutral[0] = std::byte{1};
  require(decode_contact_key_file(encode_contact_key_file(neutral)).error ==
              ContactKeyFileError::unusable_key,
          "a low-order key file was not rejected as unusable");
}

void test_low_order_key_forgery_is_refused() {
  // Under the neutral key, R = B with s = 1 satisfies the cofactored equation
  // for every message; verification must refuse the key itself.
  ContactPublicKey neutral{};
  neutral[0] = std::byte{1};
  ContactSignature forged{};
  forged[0] = std::byte{0x58};
  for (std::size_t i = 1; i < 32; ++i) forged[i] = std::byte{0x66};
  forged[32] = std::byte{1};
  require(!verify_contact_signature(neutral, bytes_of(std::string("any")),
                                    forged),
          "a forged signature under a low-order key verified");
}

void test_signature_with_unreduced_scalar_is_rejected() {
  const ContactIdentity identity = ContactIdentity::derive(secret_of(12));
  const std::string message = "scalar";
  ContactSignature signature = identity.sign(bytes_of(message));
  // s + L: the same scalar mod L, a malleated encoding.
  const auto order = from_hex<32>(
      "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
  unsigned carry = 0;
  for (std::size_t i = 0; i < 32; ++i) {
    const unsigned sum = std::to_integer<unsigned>(signature[32 + i]) +
                         std::to_integer<unsigned>(order[i]) + carry;
    signature[32 + i] = static_cast<std::byte>(sum & 0xFF);
    carry = sum >> 8;
  }
  require(!verify_contact_signature(identity.public_key(), bytes_of(message),
                                    signature),
          "a signature with s >= L verified");
}

void test_agreement_refuses_every_low_order_u() {
  const ContactIdentity identity = ContactIdentity::derive(secret_of(13));
  ContactX25519PublicKey one{};
  one[0] = std::byte{1};
  const auto minus_one = from_hex<32>(
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
  const auto order_eight = from_hex<32>(
      "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800");
  const auto order_eight_other = from_hex<32>(
      "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157");
  for (const ContactX25519PublicKey& u :
       {one, minus_one, order_eight, order_eight_other}) {
    require(!identity.agree(u), "agreement with a low-order u succeeded");
  }
}

void test_non_canonical_key_is_unusable() {
  // y = p encodes y = 0 non-canonically.
  const auto y_is_p = from_hex<32>(
      "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
  require(!contact_public_key_usable(y_is_p), "y = p was usable");
  require(decode_contact_key_file(encode_contact_key_file(y_is_p)).error ==
              ContactKeyFileError::unusable_key,
          "a non-canonical key file was accepted");
  // y = p - 1 = -1 is canonical but of order 2.
  const auto y_below_p = from_hex<32>(
      "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
  require(!contact_public_key_usable(y_below_p),
          "y = -1 (order 2) was usable");
}

void test_header_and_line_variants() {
  const ContactPublicKey key =
      ContactIdentity::derive(secret_of(14)).public_key();
  const std::string file = encode_contact_key_file(key);
  const std::string body = file.substr(file.find('\n') + 1);
  require(decode_contact_key_file("USB-VAULT-CONTACT-KEY v1 junk\r\n" + body)
                  .error == ContactKeyFileError::malformed,
          "a junk header suffix was not malformed");
  require(decode_contact_key_file("USB-VAULT-CONTACT-KEY v1x\r\n" + body)
                  .error == ContactKeyFileError::malformed,
          "a non-numeric version was not malformed");
  require(decode_contact_key_file("USB-VAULT-CONTACT-KEY v12\r\n" + body)
                  .error == ContactKeyFileError::unsupported_version,
          "a numeric version was not unsupported");

  std::string split = file;
  split.insert(file.find('\n') + 10, "\r");
  require(!decode_contact_key_file(split).public_key,
          "a CR inside the key line was accepted");
  require(!decode_contact_key_file("\xEF\xBB\xBF" + std::string(4094, ' ') +
                                   file)
               .public_key,
          "an oversized file behind a BOM was accepted");
}

void test_clone_is_independent() {
  std::optional<ContactIdentity> original = ContactIdentity::derive(secret_of(16));
  const ContactIdentity copy = original->clone();
  require(copy.public_key() == original->public_key() &&
              copy.x25519_public_key() == original->x25519_public_key(),
          "a clone has different public keys");
  original.reset();  // The copy must not depend on the original's secrets.
  const std::string message = "outlives the book";
  require(verify_contact_signature(copy.public_key(), bytes_of(message),
                                   copy.sign(bytes_of(message))),
          "a clone could not sign after the original was destroyed");
}

void test_moved_from_identity_throws() {
  ContactIdentity identity = ContactIdentity::derive(secret_of(15));
  ContactIdentity moved = std::move(identity);
  bool threw = false;
  try {
    static_cast<void>(identity.sign({}));  // NOLINT(bugprone-use-after-move)
  } catch (const std::logic_error&) {
    threw = true;
  }
  require(threw, "signing with a moved-from identity did not throw");
  require(moved.agree(contact_x25519_public_key(moved.public_key())).has_value(),
          "the moved-to identity is not usable");
}

}  // namespace

int main() {
  try {
    test_derivation_is_deterministic_and_bound_to_secret();
    test_standard_ed25519_vector();
    test_sign_and_verify();
    test_key_agreement();
    test_low_order_keys_are_unusable();
    test_fingerprint_format();
    test_key_file_round_trip_and_tolerance();
    test_key_file_rejections();
    test_low_order_key_file_is_rejected();
    test_low_order_key_forgery_is_refused();
    test_signature_with_unreduced_scalar_is_rejected();
    test_agreement_refuses_every_low_order_u();
    test_non_canonical_key_is_unusable();
    test_header_and_line_variants();
    test_moved_from_identity_throws();
    test_clone_is_independent();
  } catch (const std::exception& error) {
    std::cerr << "contact_identity_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "contact_identity_v1_tests passed\n";
  return 0;
}
