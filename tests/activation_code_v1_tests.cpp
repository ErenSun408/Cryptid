#include "usb_vault/licensing/activation_code_v1.h"
#include "usb_vault/licensing/activation_public_key.h"

#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using usb_vault::licensing::ActivationGrant;
using usb_vault::licensing::ActivationSigner;
using usb_vault::licensing::ActivationState;
using usb_vault::licensing::evaluate_activation;
using usb_vault::licensing::kClockRewindToleranceSeconds;
using usb_vault::licensing::utc_seconds_from_offset_time;
using usb_vault::licensing::verify_activation_code;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr ActivationGrant kGrant{0x89ABCDEF, 1793375999};

void test_round_trip_and_format() {
  const ActivationSigner signer = ActivationSigner::generate();
  const std::string code = signer.sign(kGrant);
  require(code.size() == 125 + 24, "code length is not 25 groups of five");
  for (std::size_t i = 5; i < code.size(); i += 6) {
    require(code[i] == '-', "groups are not hyphen separated");
  }
  require(verify_activation_code(code, signer.public_key()) == kGrant,
          "a signed code did not verify to its grant");

  std::string relaxed;
  for (const char c : code) {
    if (c == '-') { relaxed += "  \r\n"; continue; }
    relaxed += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  require(verify_activation_code(relaxed, signer.public_key()) == kGrant,
          "case, whitespace and missing hyphens were not tolerated");
}

void test_rejects_other_key_and_tampering() {
  const ActivationSigner signer = ActivationSigner::generate();
  const ActivationSigner other = ActivationSigner::generate();
  const std::string code = signer.sign(kGrant);
  require(!verify_activation_code(code, other.public_key()),
          "a code verified under another key");

  // Changing any symbol changes payload or signature bits.
  for (std::size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '-') continue;
    std::string changed = code;
    changed[i] = changed[i] == '2' ? '3' : '2';
    require(!verify_activation_code(changed, signer.public_key()),
            "a modified code still verified");
  }
}

void test_rejects_malformed_text() {
  const ActivationSigner signer = ActivationSigner::generate();
  const std::string code = signer.sign(kGrant);
  const std::vector<std::string> malformed = {
      "", code.substr(0, code.size() - 1), code + "0", code + "-00000",
      code.substr(0, 10) + "U" + code.substr(11),
      code.substr(0, 10) + "*" + code.substr(11)};
  for (const std::string& text : malformed) {
    require(!verify_activation_code(text, signer.public_key()),
            "malformed text was accepted");
  }
  // The final symbol carries one padding bit, which must be zero.
  std::string padded = code;
  const char last = padded.back();
  padded.back() = last == '1' ? '0' : '1';
  require(!verify_activation_code(padded, signer.public_key()),
          "a non-canonical final symbol was accepted");
}

void test_private_blob_round_trip_and_rejection() {
  const ActivationSigner signer = ActivationSigner::generate();
  const ActivationSigner copy =
      ActivationSigner::import_private_blob(signer.private_blob().bytes());
  require(copy.public_key() == signer.public_key(), "import changed the key");
  require(verify_activation_code(copy.sign(kGrant), signer.public_key()) ==
              kGrant,
          "imported key signed an invalid code");

  std::vector<std::byte> truncated(signer.private_blob().bytes().begin(),
                                   signer.private_blob().bytes().end() - 1);
  bool rejected = false;
  try {
    static_cast<void>(ActivationSigner::import_private_blob(truncated));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "a truncated key blob was accepted");

  std::vector<std::byte> wrong_magic(signer.private_blob().bytes().begin(),
                                     signer.private_blob().bytes().end());
  wrong_magic[0] ^= std::byte{1};
  rejected = false;
  try {
    static_cast<void>(ActivationSigner::import_private_blob(wrong_magic));
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "a key blob with the wrong magic was accepted");
}

void test_signing_rejects_out_of_range_expiry() {
  const ActivationSigner signer = ActivationSigner::generate();
  for (const std::int64_t expiry : {std::int64_t{0}, std::int64_t{-1},
                                    std::int64_t{253402300800}}) {
    bool rejected = false;
    try {
      static_cast<void>(signer.sign({1, expiry}));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "an out-of-range expiry was signed");
  }
}

void test_offset_time_conversion() {
  require(utc_seconds_from_offset_time(1970, 1, 1, 0, 0, 0, 0) == 0,
          "epoch did not convert to zero");
  // 2026-10-30 23:59:59 at UTC+8 is 15:59:59Z.
  require(utc_seconds_from_offset_time(2026, 10, 30, 23, 59, 59, 480) ==
              1793375999,
          "UTC+8 end of day converted incorrectly");
  require(utc_seconds_from_offset_time(2026, 10, 30, 15, 59, 59, 0) ==
              1793375999,
          "UTC end of day converted incorrectly");
  require(utc_seconds_from_offset_time(2026, 10, 30, 10, 29, 59, -330) ==
              1793375999,
          "negative half-hour offset converted incorrectly");
  require(utc_seconds_from_offset_time(2028, 2, 29, 0, 0, 0, 0).has_value(),
          "a leap day was rejected");
  require(utc_seconds_from_offset_time(9999, 12, 31, 23, 59, 59, 0) ==
              253402300799,
          "the latest instant converted incorrectly");

  const std::optional<std::int64_t> invalid[] = {
      utc_seconds_from_offset_time(2027, 2, 29, 0, 0, 0, 0),
      utc_seconds_from_offset_time(1900, 2, 29, 0, 0, 0, 0),
      utc_seconds_from_offset_time(2026, 4, 31, 0, 0, 0, 0),
      utc_seconds_from_offset_time(2026, 13, 1, 0, 0, 0, 0),
      utc_seconds_from_offset_time(2026, 1, 1, 24, 0, 0, 0),
      utc_seconds_from_offset_time(2026, 1, 1, 0, 60, 0, 0),
      utc_seconds_from_offset_time(2026, 1, 1, 0, 0, 60, 0),
      utc_seconds_from_offset_time(2026, 1, 1, 0, 0, 0, 14 * 60 + 15),
      utc_seconds_from_offset_time(2026, 1, 1, 0, 0, 0, -12 * 60 - 15),
      utc_seconds_from_offset_time(2026, 1, 1, 0, 0, 0, 7),
      utc_seconds_from_offset_time(10000, 1, 1, 0, 0, 0, 0)};
  for (const auto& value : invalid) {
    require(!value.has_value(), "an invalid wall-clock reading converted");
  }
}

void test_evaluation() {
  const std::optional<ActivationGrant> grant = kGrant;
  const std::uint32_t card = kGrant.card_hardware_id;
  const std::int64_t expiry = kGrant.expires_at_utc_seconds;
  const std::int64_t tolerance = kClockRewindToleranceSeconds;
  require(evaluate_activation(std::nullopt, card, 1, 0) ==
              ActivationState::missing,
          "no grant was not missing");
  require(evaluate_activation(grant, card + 1, expiry - 1, 0) ==
              ActivationState::missing,
          "a grant for another card was accepted");
  require(evaluate_activation(grant, card, expiry - 1, expiry - 1) ==
              ActivationState::active,
          "the last second before expiry was not active");
  require(evaluate_activation(grant, card, expiry, expiry) ==
              ActivationState::expired,
          "the expiry instant was still active");
  require(evaluate_activation(grant, card, 1000, 1000 + tolerance) ==
              ActivationState::active,
          "a rewind within tolerance was rejected");
  require(evaluate_activation(grant, card, 1000, 1001 + tolerance) ==
              ActivationState::clock_rewound,
          "a rewind beyond tolerance was trusted");
  require(evaluate_activation(grant, card, expiry - 1,
                              expiry + 1 + tolerance) ==
              ActivationState::clock_rewound,
          "a rewound clock was allowed to reopen an expired grant");
  for (const std::int64_t now : {std::int64_t{0}, std::int64_t{-1},
                                 std::numeric_limits<std::int64_t>::min()}) {
    require(evaluate_activation(grant, card, now, 0) ==
                ActivationState::clock_rewound,
            "a pre-1970 clock was trusted");
  }
  require(evaluate_activation(grant, card,
                              std::numeric_limits<std::int64_t>::max(),
                              std::numeric_limits<std::int64_t>::max()) ==
              ActivationState::expired,
          "the largest clock reading did not evaluate as expired");
  require(evaluate_activation(grant, card, 1,
                              std::numeric_limits<std::int64_t>::max()) ==
              ActivationState::clock_rewound,
          "the largest high-water mark did not detect a rewind");
}

void test_moved_signer_rejects_use() {
  ActivationSigner signer = ActivationSigner::generate();
  const ActivationSigner taken = std::move(signer);
  bool rejected = false;
  try {
    static_cast<void>(signer.public_key());  // NOLINT(bugprone-use-after-move)
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "a moved-from signer exposed a public key");
  require(taken.public_key().size() == 64, "the moved-to signer lost its key");
}

// A code issued by the production tool for card 0000-0000, which no card
// reports (a zero hardware ID is rejected at insertion), so it grants nothing.
// It proves the embedded public key is the issuer's.
void test_embedded_key_matches_the_issuer() {
  const auto grant = verify_activation_code(
      "040G0-00000-00000-0DB8W-PVY40-2RTB6-3SS6Q-D5XGG-KSVMB-WF543-XYDZE-1BXCA-"
      "1DMFN-NW4K5-43SQQ-MGZGV-SY981-PNVYT-YQ2Y7-XJ7WA-QJNZ8-JDVRY-PXN27-YWR4P-D7MEJ",
      usb_vault::licensing::kProductActivationPublicKey);
  require(grant.has_value() && grant->card_hardware_id == 0 &&
              grant->expires_at_utc_seconds ==
                  utc_seconds_from_offset_time(2026, 10, 15, 23, 59, 59, -420),
          "the embedded public key does not verify an issued code");
}

}  // namespace

int main() {
  try {
    test_round_trip_and_format();
    test_rejects_other_key_and_tampering();
    test_rejects_malformed_text();
    test_private_blob_round_trip_and_rejection();
    test_signing_rejects_out_of_range_expiry();
    test_offset_time_conversion();
    test_evaluation();
    test_moved_signer_rejects_use();
    test_embedded_key_matches_the_issuer();
  } catch (const std::exception& error) {
    std::cerr << "activation_code_v1_tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "activation_code_v1_tests passed\n";
  return 0;
}
