#include "usb_vault/security/password_normalization.h"

#include <Windows.h>

#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace usb_vault::security {
namespace {

static_assert(sizeof(wchar_t) == 2);

struct LockedWideText final {
  SecureBuffer storage;
  int length;

  [[nodiscard]] wchar_t* data() noexcept {
    return reinterpret_cast<wchar_t*>(storage.data());
  }

  [[nodiscard]] const wchar_t* data() const noexcept {
    return reinterpret_cast<const wchar_t*>(storage.data());
  }
};

[[noreturn]] void throw_text_error(const DWORD error,
                                   const char* const operation) {
  if (error == ERROR_NO_UNICODE_TRANSLATION) {
    throw std::invalid_argument("password must be well-formed Unicode");
  }
  throw std::system_error(
      static_cast<int>(error == ERROR_SUCCESS ? ERROR_INVALID_DATA : error),
      std::system_category(), operation);
}

[[nodiscard]] std::size_t wide_storage_bytes(const int characters) {
  if (characters <= 0 ||
      characters >
          static_cast<int>(kMaximumPasswordUtf8InputBytes)) {
    throw std::length_error("normalized password is too long");
  }
  return static_cast<std::size_t>(characters) * sizeof(wchar_t);
}

[[nodiscard]] LockedWideText utf8_to_locked_utf16(
    const SecureBuffer& password_utf8) {
  const auto* const source =
      reinterpret_cast<const char*>(password_utf8.data());
  const int source_length = static_cast<int>(password_utf8.size());

  ::SetLastError(ERROR_SUCCESS);
  const int required = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, source, source_length, nullptr, 0);
  if (required <= 0) {
    throw_text_error(::GetLastError(), "decode password UTF-8");
  }

  LockedWideText result{SecureBuffer(wide_storage_bytes(required)), required};
  ::SetLastError(ERROR_SUCCESS);
  const int converted = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, source, source_length, result.data(),
      result.length);
  if (converted != result.length) {
    throw_text_error(::GetLastError(), "decode password UTF-8");
  }
  return result;
}

[[nodiscard]] LockedWideText normalize_locked_utf16_nfc(
    const LockedWideText& source) {
  ::SetLastError(ERROR_SUCCESS);
  int capacity = ::NormalizeString(NormalizationC, source.data(),
                                   source.length, nullptr, 0);
  if (capacity <= 0) {
    throw_text_error(::GetLastError(), "estimate password NFC length");
  }

  constexpr int kMaximumAttempts = 4;
  for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    LockedWideText result{SecureBuffer(wide_storage_bytes(capacity)),
                          capacity};
    ::SetLastError(ERROR_SUCCESS);
    const int normalized = ::NormalizeString(
        NormalizationC, source.data(), source.length, result.data(), capacity);
    if (normalized > 0) {
      result.length = normalized;
      return result;
    }

    const DWORD error = ::GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || normalized >= 0) {
      throw_text_error(error, "normalize password to NFC");
    }
    const long long required = -static_cast<long long>(normalized);
    if (required <= 0 ||
        required > static_cast<long long>(
                       kMaximumPasswordUtf8InputBytes)) {
      throw std::length_error("normalized password is too long");
    }
    capacity = static_cast<int>(required);
  }

  throw std::runtime_error("password NFC normalization did not converge");
}

[[nodiscard]] SecureBuffer locked_utf16_to_utf8(
    const LockedWideText& normalized) {
  ::SetLastError(ERROR_SUCCESS);
  const int required = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(), normalized.length,
      nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    throw_text_error(::GetLastError(), "encode normalized password UTF-8");
  }
  if (required >
      static_cast<int>(kMaximumPasswordUtf8InputBytes)) {
    throw std::length_error("normalized password is too long");
  }

  SecureBuffer result(static_cast<std::size_t>(required));
  ::SetLastError(ERROR_SUCCESS);
  const int converted = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, normalized.data(), normalized.length,
      reinterpret_cast<char*>(result.data()), required, nullptr, nullptr);
  if (converted != required) {
    result.clear();
    throw_text_error(::GetLastError(), "encode normalized password UTF-8");
  }
  return result;
}

}  // namespace

SecureBuffer normalize_password_utf8_nfc(SecureBuffer password_utf8) {
  if (password_utf8.empty()) {
    throw std::invalid_argument("password must not be empty");
  }
  if (password_utf8.size() > kMaximumPasswordUtf8InputBytes) {
    throw std::length_error("password UTF-8 input is too long");
  }

  LockedWideText decoded = utf8_to_locked_utf16(password_utf8);
  password_utf8.clear();
  LockedWideText normalized = normalize_locked_utf16_nfc(decoded);
  decoded.storage.clear();
  SecureBuffer result = locked_utf16_to_utf8(normalized);
  normalized.storage.clear();
  return result;
}

}  // namespace usb_vault::security
