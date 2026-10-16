#pragma once

#include <cstdint>

namespace usb_vault::application {

// Outcome of reading the physically present card again and comparing it with
// the identity the current session authenticated with. It never carries the
// identity itself, so nothing downstream can display or record it.
enum class CardIdentityRecheck : std::uint8_t {
  same_card = 0,
  different_card = 1,
  // Nothing usable came back: absent, communication failure, or timeout.
  unreadable = 2,
  // The vendor credentials were refused. Another attempt would spend one more
  // hardware PIN try, so this is never retried.
  credentials_rejected = 3,
  // Another card operation holds the card; nothing was read.
  busy = 4,
};

class ICardIdentityVerifier {
 public:
  virtual ~ICardIdentityVerifier() noexcept = default;

  ICardIdentityVerifier(const ICardIdentityVerifier&) = delete;
  ICardIdentityVerifier& operator=(const ICardIdentityVerifier&) = delete;
  ICardIdentityVerifier(ICardIdentityVerifier&&) = delete;
  ICardIdentityVerifier& operator=(ICardIdentityVerifier&&) = delete;

  // Strictly read-only: one authenticated vendor session that reads the
  // hardware identity and closes, without touching the card record.
  [[nodiscard]] virtual CardIdentityRecheck recheck(
      std::uint32_t expected_identity) noexcept = 0;

 protected:
  ICardIdentityVerifier() noexcept = default;
};

enum class CardIdentityVerdict : std::uint8_t {
  present = 0,
  // Read once more shortly; the session stays as it is meanwhile.
  retry = 1,
  // Handle exactly as a physical removal of the authenticated card.
  lost = 2,
  // No conclusion either way; keep the previous state.
  skipped = 3,
};

// A single unreadable result may be contact bounce, so it is retried once;
// a second consecutive one counts as removal. A different card or refused
// credentials count at once. A busy card neither confirms nor clears an
// earlier unreadable result.
class CardIdentityRecheckPolicy final {
 public:
  [[nodiscard]] CardIdentityVerdict observe(CardIdentityRecheck result) noexcept;
  void reset() noexcept;

 private:
  bool unreadable_pending_{};
};

}  // namespace usb_vault::application
