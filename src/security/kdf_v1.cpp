#include "usb_vault/security/kdf_v1.h"

#include <Windows.h>

#include <argon2.h>

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace usb_vault::security {
namespace {

static_assert(kKdfV1DomainTag.size() <=
              std::numeric_limits<std::uint32_t>::max());
static_assert(kKdfV1CardSecretSize <=
              std::numeric_limits<std::uint32_t>::max());
static_assert(kKdfV1SaltSize <= std::numeric_limits<std::uint32_t>::max());
static_assert(kKdfV1OutputSize <= std::numeric_limits<std::uint32_t>::max());

std::mutex kdf_mutex;

void validate_password_utf8(const SecureBuffer& password_utf8) {
  if (password_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }

  if (password_utf8.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("password is too long for UTF-8 validation");
  }

  const auto* const password =
      reinterpret_cast<const char*>(password_utf8.data());
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

int allocate_work_area(std::uint8_t** const memory,
                       const std::size_t bytes_to_allocate) noexcept {
  if (memory == nullptr || bytes_to_allocate == 0) {
    return ARGON2_MEMORY_ALLOCATION_ERROR;
  }

  *memory = nullptr;
  void* const allocation = ::VirtualAlloc(
      nullptr, bytes_to_allocate, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    return ARGON2_MEMORY_ALLOCATION_ERROR;
  }

  *memory = static_cast<std::uint8_t*>(allocation);
  return ARGON2_OK;
}

void release_work_area(std::uint8_t* const memory,
                       const std::size_t bytes_to_release) noexcept {
  if (memory == nullptr) {
    return;
  }

  // The upstream implementation wipes the arena before invoking this
  // callback. Repeat the wipe at the ownership boundary before release.
  ::SecureZeroMemory(memory, bytes_to_release);
  static_cast<void>(::VirtualFree(memory, 0, MEM_RELEASE));
}

[[nodiscard]] std::uint8_t* mutable_bytes(
    const SecureBuffer& buffer) noexcept {
  const auto* const bytes =
      reinterpret_cast<const std::uint8_t*>(buffer.data());
  return const_cast<std::uint8_t*>(bytes);
}

[[nodiscard]] std::uint8_t* mutable_bytes(
    const KdfV1Salt& bytes) noexcept {
  const auto* const data =
      reinterpret_cast<const std::uint8_t*>(bytes.data());
  return const_cast<std::uint8_t*>(data);
}

}  // namespace

SecureBuffer derive_volume_password_v1(const SecureBuffer& password_utf8,
                                       const SecureBuffer& card_secret,
                                       const KdfV1Salt& salt) {
  validate_password_utf8(password_utf8);
  if (card_secret.size() != kKdfV1CardSecretSize) {
    throw std::invalid_argument("card secret must contain exactly 32 bytes");
  }

  SecureBuffer output(kKdfV1OutputSize);

  argon2_context context{};
  context.out = reinterpret_cast<std::uint8_t*>(output.data());
  context.outlen = static_cast<std::uint32_t>(output.size());
  context.pwd = mutable_bytes(password_utf8);
  context.pwdlen = static_cast<std::uint32_t>(password_utf8.size());
  context.salt = mutable_bytes(salt);
  context.saltlen = static_cast<std::uint32_t>(salt.size());
  context.secret = mutable_bytes(card_secret);
  context.secretlen = static_cast<std::uint32_t>(card_secret.size());
  context.ad = reinterpret_cast<std::uint8_t*>(
      const_cast<char*>(kKdfV1DomainTag.data()));
  context.adlen = static_cast<std::uint32_t>(kKdfV1DomainTag.size());
  context.t_cost = kKdfV1Iterations;
  context.m_cost = kKdfV1MemoryKib;
  context.lanes = kKdfV1Parallelism;
  context.threads = kKdfV1Parallelism;
  context.version = ARGON2_VERSION_13;
  context.allocate_cbk = &allocate_work_area;
  context.free_cbk = &release_work_area;
  context.flags = ARGON2_DEFAULT_FLAGS;

  // Bound this process to one 512 MiB work area even if callers race.
  const std::scoped_lock lock(kdf_mutex);
  const int result = ::argon2_ctx(&context, Argon2_id);
  if (result != ARGON2_OK) {
    output.clear();
    const char* const detail = ::argon2_error_message(result);
    throw std::runtime_error(std::string("Argon2id derivation failed: ") +
                             (detail == nullptr ? "unknown error" : detail));
  }

  return output;
}

}  // namespace usb_vault::security
