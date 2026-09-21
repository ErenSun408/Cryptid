#pragma once

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/volume_creation/volume_creator.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace usb_vault::volume_creation::detail {

constexpr std::uint32_t kProtocolMagic = 0x31564355U;  // "UCV1"
constexpr std::uint16_t kProtocolVersion = 3;
constexpr std::size_t kNonceSize = 16;
constexpr std::uint32_t kMaxPathCharacters = 32767;
constexpr std::uint32_t kMaxPasswordBytes = 128;

enum class Operation : std::uint32_t {
  create_standard = 1,
  create_with_hidden = 2,
  // Carries a mounted volume's root path and no secret at all.
  scan_tail_space = 3,
  scan_protected_space = 4,
};

enum class MessageType : std::uint32_t {
  progress = 1,
  result = 2,
};

#pragma pack(push, 1)
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
  std::array<std::byte, kNonceSize> nonce{};
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
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 68);
static_assert(sizeof(Response) == 36);
static_assert(sizeof(CancelRequest) == 24);

struct Request final {
  Operation operation{Operation::create_standard};
  std::uint64_t outer_size_bytes{};
  std::uint64_t hidden_size_bytes{};
  std::uint32_t mounted_drive{};
  std::int32_t mount_id{};
  std::wstring target_path;
  security::SecureBuffer first_password;
  security::SecureBuffer second_password;
  // Kept so the helper can recognise this caller's cancel while formatting.
  std::array<std::byte, kNonceSize> nonce{};
};

[[nodiscard]] bool valid_request_header(const RequestHeader& header) noexcept;
[[nodiscard]] bool valid_response(const Response& response) noexcept;

void write_exact(HANDLE pipe, const void* data, std::size_t size);
void read_exact(HANDLE pipe, void* data, std::size_t size);

void write_request(HANDLE pipe, const RequestHeader& header,
                   std::wstring_view target_path,
                   const security::SecureBuffer& first_password,
                   const security::SecureBuffer& second_password);

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
