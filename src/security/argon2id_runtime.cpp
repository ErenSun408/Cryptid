#include "security/argon2id_runtime.h"

#include <Windows.h>

#include <argon2.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace usb_vault::security::detail {
namespace {

std::mutex argon2_mutex;

void validate_password_utf8(const SecureBuffer &password_utf8) {
  if (password_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }

  if (password_utf8.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("password is too long for UTF-8 validation");
  }

  const auto *const password =
      reinterpret_cast<const char *>(password_utf8.data());
  const int length = static_cast<int>(password_utf8.size());
  ::SetLastError(ERROR_SUCCESS);
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password, length,
                            nullptr, 0) != 0) {
    return;
  }

  const DWORD error = ::GetLastError();
  if (error == ERROR_NO_UNICODE_TRANSLATION) {
    throw std::invalid_argument("password must be well-formed UTF-8");
  }
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          "MultiByteToWideChar");
}

int allocate_work_area(std::uint8_t **const memory,
                       const std::size_t bytes_to_allocate) noexcept {
  if (memory == nullptr || bytes_to_allocate == 0) {
    return ARGON2_MEMORY_ALLOCATION_ERROR;
  }

  *memory = nullptr;
  void *const allocation = ::VirtualAlloc(
      nullptr, bytes_to_allocate, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    return ARGON2_MEMORY_ALLOCATION_ERROR;
  }

  *memory = static_cast<std::uint8_t *>(allocation);
  return ARGON2_OK;
}

void release_work_area(std::uint8_t *const memory,
                       const std::size_t bytes_to_release) noexcept {
  if (memory == nullptr) {
    return;
  }

  ::SecureZeroMemory(memory, bytes_to_release);
  static_cast<void>(::VirtualFree(memory, 0, MEM_RELEASE));
}

[[nodiscard]] std::uint8_t *
mutable_bytes(const std::span<const std::byte> bytes) noexcept {
  const auto *const data = reinterpret_cast<const std::uint8_t *>(bytes.data());
  return const_cast<std::uint8_t *>(data);
}

[[nodiscard]] std::uint8_t *mutable_bytes(const SecureBuffer &buffer) noexcept {
  return mutable_bytes(buffer.bytes());
}

[[nodiscard]] std::uint8_t *
mutable_bytes(const std::string_view value) noexcept {
  const auto *const data = reinterpret_cast<const std::uint8_t *>(value.data());
  return const_cast<std::uint8_t *>(data);
}

void require_uint32_size(const std::size_t size, const char *const field) {
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error(std::string(field) + " is too large for Argon2");
  }
}

} // namespace

SecureBuffer derive_argon2id(const SecureBuffer &password_utf8,
                             const std::span<const std::byte> salt,
                             const std::span<const std::byte> secret,
                             const std::string_view domain_tag,
                             const Argon2idParameters parameters,
                             const std::size_t output_size) {
  validate_password_utf8(password_utf8);
  if (salt.empty()) {
    throw std::invalid_argument("Argon2 salt must not be empty");
  }
  if (domain_tag.empty()) {
    throw std::invalid_argument("Argon2 domain tag must not be empty");
  }
  if (parameters.memory_kib == 0 || parameters.iterations == 0 ||
      parameters.parallelism == 0) {
    throw std::invalid_argument("Argon2 parameters must be non-zero");
  }
  if (output_size == 0) {
    throw std::invalid_argument("Argon2 output must not be empty");
  }

  require_uint32_size(password_utf8.size(), "password");
  require_uint32_size(salt.size(), "salt");
  require_uint32_size(secret.size(), "secret");
  require_uint32_size(domain_tag.size(), "domain tag");
  require_uint32_size(output_size, "output");

  SecureBuffer output(output_size);
  argon2_context context{};
  context.out = reinterpret_cast<std::uint8_t *>(output.data());
  context.outlen = static_cast<std::uint32_t>(output.size());
  context.pwd = mutable_bytes(password_utf8);
  context.pwdlen = static_cast<std::uint32_t>(password_utf8.size());
  context.salt = mutable_bytes(salt);
  context.saltlen = static_cast<std::uint32_t>(salt.size());
  context.secret = secret.empty() ? nullptr : mutable_bytes(secret);
  context.secretlen = static_cast<std::uint32_t>(secret.size());
  context.ad = mutable_bytes(domain_tag);
  context.adlen = static_cast<std::uint32_t>(domain_tag.size());
  context.t_cost = parameters.iterations;
  context.m_cost = parameters.memory_kib;
  context.lanes = parameters.parallelism;
  context.threads = parameters.parallelism;
  context.version = ARGON2_VERSION_13;
  context.allocate_cbk = &allocate_work_area;
  context.free_cbk = &release_work_area;
  context.flags = ARGON2_DEFAULT_FLAGS;

  const std::scoped_lock lock(argon2_mutex);
  const int result = ::argon2_ctx(&context, Argon2_id);
  if (result != ARGON2_OK) {
    output.clear();
    const char *const detail = ::argon2_error_message(result);
    throw std::runtime_error(std::string("Argon2id derivation failed: ") +
                             (detail == nullptr ? "unknown error" : detail));
  }
  return output;
}

} // namespace usb_vault::security::detail
