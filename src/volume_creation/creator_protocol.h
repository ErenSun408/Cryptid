#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>

namespace usb_vault::volume_creation::detail {

constexpr std::uint32_t kProtocolMagic = 0x31564355U;  // "UCV1"
constexpr std::uint16_t kProtocolVersion = 11;
constexpr std::size_t kNonceSize = 16;
constexpr std::uint32_t kMaxPathCharacters = 32767;
constexpr std::uint32_t kMaxPasswordBytes = 128;

enum class Operation : std::uint32_t {
  create_standard = 1,
  create_with_hidden = 2,
  // Carries a mounted volume's root path and no secret at all.
  scan_tail_space = 3,
  scan_protected_space = 4,
  import_protected_file = 5,
  // Explicit consent for measurement and import within one bound mount.
  scan_maintenance_space = 6,
  check_partition = 7, // Read-only identity check, no formatting or password.
  format_partition = 8, // Destructive, separate confirmation required.
  begin_partition_inplace = 9,
  resume_partition_inplace = 10,
  inspect_partition_bitlocker = 11,
  decrypt_partition_bitlocker = 12,
  format_partition_pair = 13,
};

[[nodiscard]] constexpr bool inplace_operation(Operation op) noexcept {
  return op == Operation::begin_partition_inplace || op == Operation::resume_partition_inplace;
}
[[nodiscard]] constexpr bool partition_operation(Operation op) noexcept {
  return op == Operation::format_partition_pair || op == Operation::decrypt_partition_bitlocker || op == Operation::inspect_partition_bitlocker || op == Operation::check_partition || op == Operation::format_partition || inplace_operation(op);
}
[[nodiscard]] constexpr bool valid_partition_hidden_size(std::uint64_t host, std::uint64_t hidden) noexcept {
  return hidden >= 1024 * 1024 && hidden % 65536 == 0 && hidden <= host / 4 * 3;
}
// 0: continue, 1: normal pause, 2: authentication revoked/cancelled.
void write_pause_request(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce);
[[nodiscard]] unsigned inplace_control_requested(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce);

enum class MessageType : std::uint32_t {
  progress = 1,
  result = 2,
};

#pragma pack(push, 1)
// Dedicated MBR/GPT 512-byte target facts. Never reuse file sizes or hidden-volume fields.
struct PartitionTarget final {
  std::array<std::uint8_t, 16> device_id{};
  std::uint32_t device_flags{};
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  std::uint32_t sector_bytes{512};
  std::uint32_t layout{1}; // 1: MBR; 2: GPT with a nonzero partition GUID.
  std::uint32_t filesystem_serial{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint32_t reserved{};
  std::array<std::uint8_t, 16> partition_id{};
};
struct RequestHeader final {
  std::uint32_t magic{kProtocolMagic};
  std::uint16_t version{kProtocolVersion};
  std::uint16_t header_size{sizeof(RequestHeader)};
  Operation operation{Operation::create_standard};
  std::uint32_t reserved{};
  std::uint64_t outer_size_bytes{};
  std::uint64_t hidden_size_bytes{};
  std::uint32_t path_characters{};
  std::uint32_t first_password_bytes{};
  std::uint32_t second_password_bytes{};
  std::uint32_t mounted_drive{};  // 1-26 for protected scans, zero otherwise.
  std::int32_t mount_id{};
  std::uint32_t source_characters{};
  std::uint32_t destination_characters{};
  std::array<std::byte, kNonceSize> nonce{};
  std::uint32_t partition_target_bytes{};
};

struct Response final {
  std::uint32_t magic{kProtocolMagic};
  std::uint16_t version{kProtocolVersion};
  std::uint16_t size{sizeof(Response)};
  MessageType type{MessageType::result};
  CreationStage stage{CreationStage::formatting};
  std::uint32_t percent{};
  std::int32_t error{};
  std::uint32_t native_error{};
  // What the operation measured. Only a successful scan sets it.
  std::uint64_t value{};
};

// The one message that travels from caller to helper after the request. It is
// sent at most once per creation, is fixed size, and carries the same nonce as
// the request, so the helper can tell a cancel from a stray write with a
// single peek rather than by parsing whatever arrives mid-format.
struct CancelRequest final {
  std::uint32_t magic{kProtocolMagic};
  std::uint16_t version{kProtocolVersion};
  std::uint16_t size{sizeof(CancelRequest)};
  std::array<std::byte, kNonceSize> nonce{};
};
struct PartitionConfirmation final {
  std::uint32_t magic{0x31504355U}; // "UCP1", distinct from cancellation.
  std::uint16_t version{kProtocolVersion};
  std::uint16_t size{sizeof(PartitionConfirmation)};
  std::array<std::byte, kNonceSize> nonce{};
  std::uint32_t erase_all_data{}; // Exactly 0 (decline) or 1 (confirm).
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 80);
static_assert(sizeof(PartitionTarget) == 76);
static_assert(sizeof(Response) == 36);
static_assert(sizeof(CancelRequest) == 24);
static_assert(sizeof(PartitionConfirmation) == 28);

struct Request final {
  Operation operation{Operation::create_standard};
  std::uint64_t outer_size_bytes{};
  std::uint64_t hidden_size_bytes{};
  std::uint32_t mounted_drive{};
  std::int32_t mount_id{};
  std::wstring target_path;
  std::wstring source_path;
  std::wstring destination_relative;
  security::SecureBuffer first_password;
  security::SecureBuffer second_password;
  // Kept so the helper can recognise this caller's cancel while formatting.
  std::array<std::byte, kNonceSize> nonce{};
  std::optional<PartitionTarget> partition_target;
};

void write_partition_confirmation(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce, bool confirmed);
[[nodiscard]] bool read_partition_confirmation(HANDLE pipe, const std::array<std::byte, kNonceSize>& nonce);

[[nodiscard]] bool valid_partition_target(const PartitionTarget& target) noexcept;
[[nodiscard]] bool valid_partition_path(std::wstring_view path) noexcept;
[[nodiscard]] bool valid_request_header(const RequestHeader& header) noexcept;
[[nodiscard]] bool valid_response(const Response& response) noexcept;

void write_exact(HANDLE pipe, const void* data, std::size_t size);
void read_exact(HANDLE pipe, void* data, std::size_t size);

void write_request(HANDLE pipe, const RequestHeader& header,
                   std::wstring_view target_path,
                   const security::SecureBuffer& first_password,
                   const security::SecureBuffer& second_password,
                   std::wstring_view source_path = {},
                   std::wstring_view destination_relative = {},
                   const PartitionTarget* partition_target = nullptr);

[[nodiscard]] Request read_request(
    HANDLE pipe, const std::array<std::byte, kNonceSize>& expected_nonce);

void write_response(HANDLE pipe, const Response& response);
[[nodiscard]] Response read_response(HANDLE pipe);

[[nodiscard]] bool valid_cancel_request(
    const CancelRequest& request,
    const std::array<std::byte, kNonceSize>& expected_nonce) noexcept;

void write_cancel_request(HANDLE pipe,
                          const std::array<std::byte, kNonceSize>& nonce);

// Answers whether this caller has asked for the creation to be abandoned, and
// never waits for one: the helper calls it from inside the format backend's
// progress callback, where blocking would stall the very format it is being
// asked about.
[[nodiscard]] bool cancel_requested(
    HANDLE pipe, const std::array<std::byte, kNonceSize>& expected_nonce);

}  // namespace usb_vault::volume_creation::detail
