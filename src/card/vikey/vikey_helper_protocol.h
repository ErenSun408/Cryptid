#pragma once

#include "card/verified_local_artifact.h"
#include "card/vikey/vikey_session.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace usb_vault::card::vikey::helper {

constexpr std::uint32_t kMagic = 0x50484b56U;  // "VKHP" little-endian.
constexpr std::uint16_t kVersion = 14;
constexpr std::size_t kNonceSize = 16;
constexpr std::uint32_t kHardwareIdentityBytes = sizeof(std::uint32_t);
constexpr std::uint32_t kCardMediaStateBytes = sizeof(std::uint32_t);
constexpr std::uint32_t kDevelopmentSessionPayloadBytes =
    static_cast<std::uint32_t>(Sha256Digest{}.size() + kVikeyUserSnBytes +
                               kVikeyApiKeyBytes + kVikeyUserPinBytes);
constexpr std::uint32_t kDevelopmentBlobRoundTripPayloadBytes =
    kDevelopmentSessionPayloadBytes +
    static_cast<std::uint32_t>(kVikeyBlobBytes);
constexpr std::uint32_t kDevelopmentBlobReadPayloadBytes =
    kDevelopmentSessionPayloadBytes;
constexpr std::uint32_t kDevelopmentBlobWritePayloadBytes =
    kDevelopmentSessionPayloadBytes +
    static_cast<std::uint32_t>(kVikeyBlobBytes);
constexpr std::uint32_t kCardRecordReadPayloadBytes =
    kDevelopmentSessionPayloadBytes + kHardwareIdentityBytes;
constexpr std::uint32_t kCardRecordWritePayloadBytes =
    kCardRecordReadPayloadBytes +
    static_cast<std::uint32_t>(kCardRecordV1Size);
constexpr std::uint32_t kCardIdentityProbePayloadBytes =
    kDevelopmentSessionPayloadBytes;
constexpr std::uint32_t kCardMediaProbePayloadBytes =
    kDevelopmentSessionPayloadBytes;
constexpr std::uint32_t kCardMediaBootstrapPayloadBytes =
    kDevelopmentSessionPayloadBytes + kHardwareIdentityBytes +
    kCardMediaStateBytes;
constexpr std::uint32_t kDevelopmentMediaResetPayloadBytes =
    kCardMediaBootstrapPayloadBytes;

enum class Operation : std::uint32_t {
  probe = 1,
  vendor_runtime_probe = 2,
  hid_access_probe = 3,
  development_session_probe = 4,
  development_blob_round_trip_probe = 5,
  development_blob_read = 6,
  development_blob_write = 7,
  card_record_read = 8,
  card_record_write = 9,
  card_identity_probe = 10,
  card_media_probe = 11,
  card_media_bootstrap = 12,
  development_media_reset = 13,
  card_record_snapshot = 14,
  card_record_reset = 15,
};

[[nodiscard]] constexpr bool is_record_read(Operation op) noexcept {
  return op == Operation::card_record_read || op == Operation::card_record_snapshot;
}
[[nodiscard]] constexpr bool is_record_write(Operation op) noexcept {
  return op == Operation::card_record_write || op == Operation::card_record_reset;
}

enum class CardMediaState : std::uint32_t {
  blank = 1,
  legacy_p1_or_foreign_prefix = 2,
  foreign_data = 3,
  provisioning_interrupted = 4,
  conflicting_evidence = 5,
  product_evidence = 6,
};

static_assert(sizeof(CardMediaState) == kCardMediaStateBytes);

// Public logical-record framing returned only alongside product_evidence.
// Values are explicit wire constants and do not serialize the C++ enum.
enum class CardLogicalState : std::uint32_t {
  provisioned_uninitialized = 1,
  initialized_v1 = 2,
  unsupported_version = 3,
  malformed = 4,
};

[[nodiscard]] constexpr bool valid_card_logical_state(
    const std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(
                      CardLogicalState::provisioned_uninitialized) &&
         value <= static_cast<std::uint32_t>(CardLogicalState::malformed);
}

[[nodiscard]] constexpr bool valid_card_media_state(
    const std::uint32_t value) noexcept {
  return value >= static_cast<std::uint32_t>(CardMediaState::blank) &&
         value <= static_cast<std::uint32_t>(
                      CardMediaState::product_evidence);
}

[[nodiscard]] constexpr bool valid_bootstrap_media_state(
    const CardMediaState state) noexcept {
  return state == CardMediaState::blank ||
         state == CardMediaState::provisioning_interrupted;
}

[[nodiscard]] constexpr bool valid_development_reset_media_state(
    const CardMediaState state) noexcept {
  return state == CardMediaState::legacy_p1_or_foreign_prefix;
}

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

// The zero-network AppContainer boundary still requires every property.
constexpr std::uint32_t kRequiredSecurityProperties =
    kKnownSecurityProperties;

// Development compatibility deliberately omits the AppContainer boundary so
// the vendor runtime can use ordinary network and HID access. It is not a
// production card-session boundary.
constexpr std::uint32_t kDevelopmentCompatibilitySecurityProperties =
    kRequiredSecurityProperties &
    ~property_bit(SecurityProperty::app_container) &
    ~property_bit(SecurityProperty::no_capabilities) &
    ~property_bit(SecurityProperty::low_integrity) &
    ~property_bit(SecurityProperty::not_elevated);

// Compatibility mode may inherit an elevated parent. Keep reporting the actual
// elevation observation, but require every other property and reject extra bits.
constexpr std::uint32_t kDevelopmentOptionalSecurityProperties =
    property_bit(SecurityProperty::not_elevated);

[[nodiscard]] constexpr bool security_properties_match(
    const std::uint32_t actual, const std::uint32_t required,
    const std::uint32_t optional = 0) noexcept {
  return (actual & ~optional) == required;
}

struct Request final {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kVersion};
  std::uint16_t header_bytes{sizeof(Request)};
  Operation operation{Operation::probe};
  std::uint32_t payload_bytes{};
  std::array<std::byte, kNonceSize> nonce{};
};

// The non-secret digest remains ordinary fixed storage. All credentials and
// development blob data are allocated as locked buffers and wiped by their
// destructors.
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

struct DevelopmentBlobRoundTripRequest final {
  DevelopmentBlobRoundTripRequest();

  DevelopmentBlobRoundTripRequest(
      const DevelopmentBlobRoundTripRequest&) = delete;
  DevelopmentBlobRoundTripRequest& operator=(
      const DevelopmentBlobRoundTripRequest&) = delete;
  DevelopmentBlobRoundTripRequest(
      DevelopmentBlobRoundTripRequest&&) noexcept = default;
  DevelopmentBlobRoundTripRequest& operator=(
      DevelopmentBlobRoundTripRequest&&) noexcept = default;

  Sha256Digest vendor_digest{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
  security::SecureBuffer test_blob;
};

struct DevelopmentBlobWriteRequest final {
  DevelopmentBlobWriteRequest();

  DevelopmentBlobWriteRequest(const DevelopmentBlobWriteRequest&) = delete;
  DevelopmentBlobWriteRequest& operator=(
      const DevelopmentBlobWriteRequest&) = delete;
  DevelopmentBlobWriteRequest(DevelopmentBlobWriteRequest&&) noexcept =
      default;
  DevelopmentBlobWriteRequest& operator=(
      DevelopmentBlobWriteRequest&&) noexcept = default;

  Sha256Digest vendor_digest{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
  security::SecureBuffer blob;
};

struct CardRecordSessionRequest final {
  CardRecordSessionRequest();

  CardRecordSessionRequest(const CardRecordSessionRequest&) = delete;
  CardRecordSessionRequest& operator=(const CardRecordSessionRequest&) =
      delete;
  CardRecordSessionRequest(CardRecordSessionRequest&&) noexcept = default;
  CardRecordSessionRequest& operator=(CardRecordSessionRequest&&) noexcept =
      default;

  Sha256Digest vendor_digest{};
  std::uint32_t expected_hardware_id{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
};

struct CardMediaBootstrapRequest final {
  CardMediaBootstrapRequest();

  CardMediaBootstrapRequest(const CardMediaBootstrapRequest&) = delete;
  CardMediaBootstrapRequest& operator=(const CardMediaBootstrapRequest&) =
      delete;
  CardMediaBootstrapRequest(CardMediaBootstrapRequest&&) noexcept = default;
  CardMediaBootstrapRequest& operator=(CardMediaBootstrapRequest&&) noexcept =
      default;

  Sha256Digest vendor_digest{};
  std::uint32_t expected_hardware_id{};
  CardMediaState expected_media_state{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
};

struct CardRecordWriteRequest final {
  CardRecordWriteRequest();

  CardRecordWriteRequest(const CardRecordWriteRequest&) = delete;
  CardRecordWriteRequest& operator=(const CardRecordWriteRequest&) = delete;
  CardRecordWriteRequest(CardRecordWriteRequest&&) noexcept = default;
  CardRecordWriteRequest& operator=(CardRecordWriteRequest&&) noexcept =
      default;

  Sha256Digest vendor_digest{};
  std::uint32_t expected_hardware_id{};
  security::SecureBuffer user_sn;
  security::SecureBuffer api_key;
  security::SecureBuffer user_pin;
  security::SecureBuffer record;
};

struct DevelopmentSessionObservation final {
  std::uint8_t attempts_before{};
  std::uint8_t maximum_before{};
  std::uint8_t attempts_after{};
  std::uint8_t maximum_after{};
};

[[nodiscard]] constexpr bool valid_development_session_observation(
    const DevelopmentSessionObservation& observation) noexcept {
  const bool before_valid = observation.maximum_before == 0 ||
                            observation.attempts_before <=
                                observation.maximum_before;
  const bool after_valid = observation.maximum_after == 0 ||
                           observation.attempts_after <=
                               observation.maximum_after;
  return before_valid && after_valid;
}

[[nodiscard]] constexpr std::uint32_t pack_development_session_observation(
    const DevelopmentSessionObservation& observation) noexcept {
  return static_cast<std::uint32_t>(observation.attempts_before) |
         (static_cast<std::uint32_t>(observation.maximum_before) << 8U) |
         (static_cast<std::uint32_t>(observation.attempts_after) << 16U) |
         (static_cast<std::uint32_t>(observation.maximum_after) << 24U);
}

[[nodiscard]] constexpr DevelopmentSessionObservation
unpack_development_session_observation(const std::uint32_t packed) noexcept {
  return {
      .attempts_before = static_cast<std::uint8_t>(packed & 0xffU),
      .maximum_before = static_cast<std::uint8_t>((packed >> 8U) & 0xffU),
      .attempts_after = static_cast<std::uint8_t>((packed >> 16U) & 0xffU),
      .maximum_after = static_cast<std::uint8_t>((packed >> 24U) & 0xffU),
  };
}

struct Response final {
  std::uint32_t magic{kMagic};
  std::uint16_t version{kVersion};
  std::uint16_t header_bytes{sizeof(Response)};
  std::int32_t status{};
  std::uint32_t security_properties{};
  std::uint32_t payload_bytes{};
  // Non-secret Win32 or vendor status on failure. A successful development
  // session packs its before/after PIN-attempt observation here; identity and
  // media probes return the authenticated card's non-secret hardware ID.
  // It never carries paths, credentials, or card data.
  std::uint32_t detail{};
  // A successful card-media probe returns a validated CardMediaState here.
  // All other responses keep this field zero.
  std::uint32_t result{};
  // A successful card-media probe returns a validated CardLogicalState only
  // when result is product_evidence. Every other response keeps this zero.
  std::uint32_t logical_state{};
  std::uint32_t diagnostic{};
  std::uint32_t native_kind{};
  // Only helper environment rejection steps 90..108; zero on all other responses.
  std::uint32_t security_step{};
  std::array<std::byte, kNonceSize> nonce{};
};

static_assert(sizeof(Request) == 32);
static_assert(sizeof(Response) == 60);

[[nodiscard]] bool valid_request(const Request& request) noexcept;
[[nodiscard]] bool valid_response(const Response& response,
                                  Operation expected_operation) noexcept;

void write_request(HANDLE pipe, const Request& request);
[[nodiscard]] Request read_request(HANDLE pipe);
void write_development_session_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] DevelopmentSessionRequest read_development_session_request(
    HANDLE pipe, const Request& request);
void write_card_identity_probe_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] DevelopmentSessionRequest read_card_identity_probe_request(
    HANDLE pipe, const Request& request);
void write_card_media_probe_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] DevelopmentSessionRequest read_card_media_probe_request(
    HANDLE pipe, const Request& request);
void write_card_media_bootstrap_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    std::uint32_t expected_hardware_id,
    CardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] CardMediaBootstrapRequest read_card_media_bootstrap_request(
    HANDLE pipe, const Request& request);
void write_development_media_reset_request(
    HANDLE pipe, const Request& request,
    const Sha256Digest& vendor_digest,
    std::uint32_t expected_hardware_id,
    CardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] CardMediaBootstrapRequest
read_development_media_reset_request(HANDLE pipe,
                                     const Request& request);
void write_development_blob_round_trip_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& test_blob);
[[nodiscard]] DevelopmentBlobRoundTripRequest
read_development_blob_round_trip_request(HANDLE pipe,
                                         const Request& request);
void write_development_blob_read_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] DevelopmentSessionRequest read_development_blob_read_request(
    HANDLE pipe, const Request& request);
void write_development_blob_write_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& blob);
[[nodiscard]] DevelopmentBlobWriteRequest
read_development_blob_write_request(HANDLE pipe, const Request& request);
void write_card_record_read_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin);
[[nodiscard]] CardRecordSessionRequest read_card_record_read_request(
    HANDLE pipe, const Request& request);
void write_card_record_write_request(
    HANDLE pipe, const Request& request, const Sha256Digest& vendor_digest,
    std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& record);
[[nodiscard]] CardRecordWriteRequest read_card_record_write_request(
    HANDLE pipe, const Request& request);
void write_response(HANDLE pipe, const Response& response,
                    Operation expected_operation);
void write_development_blob_response(
    HANDLE pipe, const Response& response,
    const security::SecureBuffer& blob);
void write_card_record_response(HANDLE pipe, const Response& response,
                                const security::SecureBuffer& record);
[[nodiscard]] Response read_response(
    HANDLE pipe, const std::array<std::byte, kNonceSize>& expected_nonce,
    Operation expected_operation);
[[nodiscard]] security::SecureBuffer read_development_blob_response_payload(
    HANDLE pipe, const Response& response);
[[nodiscard]] security::SecureBuffer read_card_record_response_payload(
    HANDLE pipe, const Response& response);

}  // namespace usb_vault::card::vikey::helper
