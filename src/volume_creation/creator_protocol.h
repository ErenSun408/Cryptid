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
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::size_t kNonceSize = 16;
constexpr std::uint32_t kMaxPathCharacters = 32767;
constexpr std::uint32_t kMaxPasswordBytes = 128;

enum class Operation : std::uint32_t {
  create_standard = 1,
  create_with_hidden = 2,
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
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 60);
static_assert(sizeof(Response) == 28);

struct Request final {
  Operation operation{Operation::create_standard};
  std::uint64_t outer_size_bytes{};
  std::uint64_t hidden_size_bytes{};
  std::wstring target_path;
  security::SecureBuffer first_password;
  security::SecureBuffer second_password;
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

}  // namespace usb_vault::volume_creation::detail
