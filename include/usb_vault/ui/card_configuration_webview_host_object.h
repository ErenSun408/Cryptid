#pragma once

#include <oaidl.h>

#include <cstddef>
#include <span>

namespace usb_vault::ui {

// What the setup page is allowed to learn about stored credentials. Neither
// the values nor the underlying Windows error are exposed.
enum class ConfigurationCredentialStatus : unsigned int {
  ready = 0,
  missing = 1,
  unreadable = 2,
};

// Outcome of one save request. A failed read-back is a storage failure, never
// a success: the page must not report saved credentials it cannot re-read.
enum class ConfigurationOutcome : unsigned int {
  saved = 0,
  invalid_format = 1,
  storage_failure = 2,
};

// Vendor-neutral seam for the in-product setup page. The host owns credential
// storage and the product restart; this bridge only carries validated bytes
// across and wipes every copy it made.
class IConfigurationHost {
 public:
  virtual ~IConfigurationHost() noexcept = default;

  IConfigurationHost(const IConfigurationHost&) = delete;
  IConfigurationHost& operator=(const IConfigurationHost&) = delete;
  IConfigurationHost(IConfigurationHost&&) = delete;
  IConfigurationHost& operator=(IConfigurationHost&&) = delete;

  [[nodiscard]] virtual ConfigurationCredentialStatus
  credential_status() noexcept = 0;
  // Receives the three values as one set. A partially valid set is rejected
  // whole, so a stored set is never a mix of old and new values.
  [[nodiscard]] virtual ConfigurationOutcome save_credentials(
      std::span<const std::byte> user_sn,
      std::span<const std::byte> api_key,
      std::span<const std::byte> user_pin) noexcept = 0;
  // Leaves setup for the product session. It is offered at every moment
  // setup is usable, so an unconfigured first run is never a page the user
  // cannot leave; a host that cannot make the change says so by returning
  // false. Closing the window is its own affair, so it is not a member here.
  [[nodiscard]] virtual bool continue_to_session() noexcept = 0;

 protected:
  IConfigurationHost() noexcept = default;
};

// Creates the setup-only automation surface for the local page. It exposes no
// card, volume, container or session member, so a page shown before any
// credential exists cannot reach the product or touch the security card.
HRESULT create_card_configuration_webview_host_object(
    IConfigurationHost& host, IDispatch** result) noexcept;

}  // namespace usb_vault::ui
