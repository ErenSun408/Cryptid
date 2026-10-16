#pragma once

#include "usb_vault/licensing/activation_code_v1.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace usb_vault::application {

enum class ActivationInstallResult : std::uint8_t {
  accepted = 0,
  invalid = 1,
  other_card = 2,
  expired = 3,
  clock_rewound = 4,
};

// What the product consults before it lets a card initialize or log in. The
// card identity is the ViKey hardware ID of the present card; it is used only
// to select that card's grant.
class IActivationGate {
 public:
  virtual ~IActivationGate() noexcept = default;
  IActivationGate(const IActivationGate&) = delete;
  IActivationGate& operator=(const IActivationGate&) = delete;
  IActivationGate(IActivationGate&&) = delete;
  IActivationGate& operator=(IActivationGate&&) = delete;

  // Fails closed: any internal failure reports missing.
  [[nodiscard]] virtual licensing::ActivationState state(
      std::uint32_t card_hardware_id) noexcept = 0;
  [[nodiscard]] virtual std::optional<std::int64_t> expiry(
      std::uint32_t card_hardware_id) noexcept = 0;
  // Verifies and, only when the grant is for this card and currently usable,
  // keeps it in place of that card's earlier grant.
  [[nodiscard]] virtual ActivationInstallResult install(
      std::uint32_t card_hardware_id, std::string_view code) = 0;

 protected:
  IActivationGate() noexcept = default;
};

// Local activation record: the codes of cards activated on this Windows
// account and the latest clock reading seen, sealed with DPAPI for the
// current user. It holds no password, container, mode or tier information.
// A missing, foreign or unreadable record reads as empty; deleting it is the
// accepted way around the rewind check, as is any offline licence.
class ActivationStoreV1 final : public IActivationGate {
 public:
  using Clock = std::function<std::int64_t()>;

  ActivationStoreV1(std::wstring path, const licensing::ActivationPublicKey& key,
                    Clock clock = system_utc_seconds);

  [[nodiscard]] licensing::ActivationState state(
      std::uint32_t card_hardware_id) noexcept override;
  [[nodiscard]] std::optional<std::int64_t> expiry(
      std::uint32_t card_hardware_id) noexcept override;
  [[nodiscard]] ActivationInstallResult install(
      std::uint32_t card_hardware_id, std::string_view code) override;

  [[nodiscard]] static std::int64_t system_utc_seconds() noexcept;
  // %LOCALAPPDATA%\usb-vault\activation.dat
  [[nodiscard]] static std::wstring default_path();

 private:
  struct Entry final {
    licensing::ActivationGrant grant;
    std::string code;
  };

  // Returns false while an existing record cannot be read; nothing may be
  // written over it then, and the next call tries again.
  bool load_once();
  // Raises the high-water mark to now and returns the reading used.
  std::int64_t observe();
  void save();

  std::wstring path_;
  licensing::ActivationPublicKey key_;
  Clock clock_;
  std::mutex mutex_;
  bool loaded_{};
  std::int64_t highest_{};
  std::int64_t saved_highest_{};
  std::map<std::uint32_t, Entry> entries_;
};

}  // namespace usb_vault::application
