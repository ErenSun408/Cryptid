#pragma once

#include "card/verified_local_artifact.h"
#include "card/vikey/vikey_session.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb_vault::card::vikey::helper {

constexpr std::uint32_t kMagic = 0x50484b56U;  // "VKHP" little-endian.
constexpr std::uint16_t kVersion = 2;
constexpr std::size_t kNonceSize = 16;
constexpr std::uint32_t kDevelopmentSessionPayloadBytes =
    static_cast<std::uint32_t>(Sha256Digest{}.size() + kVikeyUserSnBytes +
                               kVikeyApiKeyBytes + kVikeyUserPinBytes);

enum class Operation : std::uint32_t {
  probe = 1,
  vendor_runtime_probe = 2,
  hid_access_probe = 3,
  development_session_probe = 4,
};

enum class SecurityProperty : std::uint32_t {
  filtered_token = 1U << 0,
  not_elevated = 1U << 1,
  assigned_to_job = 1U << 2,
  child_processes_disabled = 1U << 3,
  dep_enabled = 1U << 4,
  bottom_up_aslr_enabled = 1U << 5,
  high_entropy_aslr_enabled = 1U << 6,
  strict_handle_checks = 1U << 7,
  extension_points_disabled = 1U << 8,
  remote_image_load_blocked = 1U << 9,
  low_label_image_load_blocked = 1U << 10,
  sanitized_environment = 1U << 11,
  app_container = 1U << 12,
  no_capabilities = 1U << 13,
  low_integrity = 1U << 14,
};

[[nodiscard]] constexpr std::uint32_t property_bit(
    const SecurityProperty property) noexcept {
  return static_cast<std::uint32_t>(property);
}

constexpr std::uint32_t kKnownSecurityProperties =
    property_bit(SecurityProperty::filtered_token) |
    property_bit(SecurityProperty::not_elevated) |
    property_bit(SecurityProperty::assigned_to_job) |
    property_bit(SecurityProperty::child_processes_disabled) |
    property_bit(SecurityProperty::dep_enabled) |
    property_bit(SecurityProperty::bottom_up_aslr_enabled) |
    property_bit(SecurityProperty::high_entropy_aslr_enabled) |
    property_bit(SecurityProperty::strict_handle_checks) |
    property_bit(SecurityProperty::extension_points_disabled) |
    property_bit(SecurityProperty::remote_image_load_blocked) |
    property_bit(SecurityProperty::low_label_image_load_blocked) |
    property_bit(SecurityProperty::sanitized_environment) |
    property_bit(SecurityProperty::app_container) |
    property_bit(SecurityProperty::no_capabilities) |
    property_bit(SecurityProperty::low_integrity);

// All properties are mandatory before a helper may be used for a card session.
constexpr std::uint32_t kRequiredSecurityProperties =
    kKnownSecurityProperties;

// Development compatibility deliberately omits the AppContainer boundary so
// the vendor runtime can use ordinary network and HID access. It is not a
// production card-session boundary.
constexpr std::uint32_t kDevelopmentCompatibilitySecurityProperties =
    kRequiredSecurityProperties &
    ~property_bit(SecurityProperty::app_container) &
    ~property_bit(SecurityProperty::no_capabilities) &
    ~property_bit(SecurityProperty::low_integrity);

struct Request final {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kVersion};
  std::uint16_t header_bytes{sizeof(Request)};
  Operation operation{Operation::probe};
  std::uint32_t payload_bytes{};
  std::array<std::byte, kNonceSize> nonce{};
};

// The non-secret digest remains ordinary fixed storage. All credentials are
// allocated as locked buffers and are wiped by their destructors.
struct DevelopmentSessionRequest final {
  DevelopmentSessionRequest();

  DevelopmentSessionRequest(const DevelopmentSessionRequest&) = delete;
  DevelopmentSessionRequest& operator=(const DevelopmentSessionRequest&) =
      delete;
  DevelopmentSessionRequest(DevelopmentSessionRequest&&) noexcept = default;
  DevelopmentSessionRequest& operator=(DevelopmentSessionRequest&&) noexcept =
      default;

  Sha256Digest vendor_digest{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
};

struct Response final {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kVersion};
  std::uint16_t header_bytes{sizeof(Response)};
  std::int32_t status{};
  std::uint32_t security_properties{};
  // Non-secret Win32 or vendor status for compatibility diagnostics. It is
  // zero on success and never carries paths, identifiers, or credentials.
  std::uint32_t detail{};
  std::array<std::byte, kNonceSize> nonce{};
};

static_assert(sizeof(Request) == 32);
static_assert(sizeof(Response) == 36);

[[nodiscard]] bool valid_request(const Request& request) noexcept;
[[nodiscard]] bool valid_response(const Response& response) noexcept;

void write_request(HANDLE pipe, const Request& request);
[[nodiscard]] Request read_request(HANDLE pipe);
void write_development_session_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] DevelopmentSessionRequest read_development_session_request(
    HANDLE pipe, const Request& request);
void write_response(HANDLE pipe, const Response& response);
[[nodiscard]] Response read_response(
    HANDLE pipe, const std::array<std::byte, kNonceSize>& expected_nonce);

}  // namespace usb_vault::card::vikey::helper
