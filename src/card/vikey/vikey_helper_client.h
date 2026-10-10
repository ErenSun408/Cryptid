#pragma once

#include "card/verified_local_artifact.h"
#include "card/vikey/vikey_card_record_io.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace usb_vault::card::vikey {

struct VikeyHelperProbe final {
  std::uint32_t security_properties{};
};

struct VikeyDevelopmentSessionProbe final {
  std::uint32_t security_properties{};
  std::uint8_t attempts_before{};
  std::uint8_t maximum_before{};
  std::uint8_t attempts_after{};
  std::uint8_t maximum_after{};
};

struct VikeyCardIdentityProbe final {
  std::uint32_t security_properties{};
  std::uint32_t hardware_id{};
};

struct VikeyCardMediaProbe final {
  std::uint32_t security_properties{};
  std::uint32_t hardware_id{};
  VikeyCardMediaState media_state{VikeyCardMediaState::foreign_data};
  std::optional<CardRecordState> logical_state;
};

// Verifies the helper against the required digest, holds its file identity
// locked, launches it under the mandatory process boundary, and performs the
// current versioned probe handshake. This does not load ViKey.dll or access a
// card.
[[nodiscard]] VikeyHelperProbe probe_vikey_helper(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Copies a verified, fixed-name ViKey.dll into the ephemeral AppContainer
// profile, locks the staged bytes against replacement, and verifies that the
// helper can load it using only its own directory and System32 dependencies.
// No SDK call other than VikeyGetLibraryVersion is made.
[[nodiscard]] VikeyHelperProbe probe_vikey_vendor_runtime(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Verifies that the isolated helper can enumerate the fixed ViKeyPRO HID and
// obtain a read/write handle without sending any HID report or SDK command.
[[nodiscard]] VikeyHelperProbe probe_vikey_hid_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only compatibility entry points. These retain a privilege-
// filtered, non-elevated process token, artifact locks, handle allowlist, job,
// mitigations, sanitized environment, IPC validation, and timeout, but
// deliberately do not create an AppContainer. The helper therefore has
// ordinary network and HID access and must never receive production material.
[[nodiscard]] VikeyHelperProbe
probe_vikey_development_helper_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

[[nodiscard]] VikeyHelperProbe
probe_vikey_development_vendor_runtime_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

[[nodiscard]] VikeyHelperProbe
probe_vikey_development_hid_access_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only one-shot authentication. Credentials travel only through
// the authenticated helper pipe into locked helper buffers; the helper loads
// the verified staged SDK, opens and closes one user session, then exits.
// This boundary has ordinary network access and must never receive production
// credentials or material.
[[nodiscard]] VikeyDevelopmentSessionProbe
probe_vikey_development_session_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Establishes the nonzero stable hardware identity that later card-record
// operations must present. It authenticates and closes one helper session but
// does not read or write the card user-data area.
[[nodiscard]] VikeyCardIdentityProbe
probe_vikey_development_card_identity_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Establishes one insertion context atomically: the helper authenticates one
// stable card, classifies its user-data area without writing, returns that
// hardware identity and state, closes the session, and exits.
[[nodiscard]] VikeyCardMediaProbe
probe_vikey_development_card_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only destructive transition for an insertion context returned
// by the media probe. The helper binds the same nonzero hardware identity,
// revalidates the exact eligible state, establishes the recoverable marker
// journal, closes the session, and exits. It never accepts card passwords or
// creates an initialized envelope record.
void bootstrap_vikey_development_card_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::uint32_t expected_hardware_id,
    VikeyCardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only one-shot reset for the exact P1-shaped media state. The
// helper binds the expected hardware identity, revalidates the complete card
// media shape, verifies a full-area clear, and returns only after the card is
// classified blank. It never accepts foreign or existing product media.
void reset_vikey_development_legacy_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::uint32_t expected_hardware_id,
    VikeyCardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only destructive probe. The helper backs up the fixed blob,
// writes and verifies test_blob, restores the original, verifies restoration,
// closes the session, and exits. Failure to prove restoration is reported as
// device_failure. The caller must supply only disposable non-production data.
[[nodiscard]] VikeyHelperProbe
probe_vikey_development_blob_round_trip_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& test_blob,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only one-shot blob operations used by the formal card-store
// backend. A read returns card data directly in locked memory. A write never
// retries; any reported write failure must be treated as an indeterminate card
// state by the caller. These operations retain ordinary network access and
// must not receive production credentials or material.
[[nodiscard]] security::SecureBuffer
read_vikey_development_blob_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

void write_vikey_development_blob_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& blob,
    std::chrono::milliseconds timeout = std::chrono::seconds(5));

// Development-only one-shot journaled card-record operations. The helper
// validates the fixed public framing and performs the recoverable 216-byte
// journal transaction in one authenticated session. A write never retries;
// callers must treat any reported failure as an indeterminate card state.
// This boundary has ordinary network access and must not receive production
// credentials or material.
[[nodiscard]] security::SecureBuffer
read_vikey_development_card_record_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    std::chrono::milliseconds timeout = std::chrono::seconds(5),
    const bool snapshot = false);

void write_vikey_development_card_record_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& record,
    std::chrono::milliseconds timeout = std::chrono::seconds(5),
    const bool reset = false);

}  // namespace usb_vault::card::vikey
