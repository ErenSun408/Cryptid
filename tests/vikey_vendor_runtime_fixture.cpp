#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>

namespace {

constexpr DWORD kInvalidValue = 0x80000005U;

enum class SessionState {
  initial,
  user_sn_set,
  found,
  api_key_set,
  attempts_read,
  logged_in,
  level_read,
  attempts_rechecked,
  backup_size_read,
  backup_read,
  test_write_size_read,
  test_written,
  test_read_size_read,
  test_verified,
  restore_write_size_read,
  restored,
  restore_read_size_read,
  restore_verified,
  logged_off,
  complete,
};

SessionState session_state = SessionState::initial;
std::array<BYTE, 32> user_data = [] {
  std::array<BYTE, 32> result{};
  result.fill(0x3c);
  return result;
}();

[[nodiscard]] bool fixed_argument_matches(const char* const value,
                                          const char expected,
                                          const std::size_t bytes) noexcept {
  if (value == nullptr) return false;
  for (std::size_t index = 0; index < bytes; ++index) {
    if (value[index] != expected) return false;
  }
  return value[bytes] == '\0';
}

}  // namespace

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetLibraryVersion(char* const version) {
  if (version == nullptr) return ERROR_INVALID_PARAMETER;
  constexpr char kVersion[] = "fixture-1";
  std::copy(std::begin(kVersion), std::end(kVersion), version);
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeySetUserSN(char* const user_sn) {
  if (session_state != SessionState::initial ||
      !fixed_argument_matches(user_sn, 'A', 32)) {
    return kInvalidValue;
  }
  session_state = SessionState::user_sn_set;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyFindEx(DWORD* const count) {
  if (session_state != SessionState::user_sn_set || count == nullptr) {
    return kInvalidValue;
  }
  *count = 1;
  session_state = SessionState::found;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeySetApiKey(const WORD index, char* const api_key) {
  if (session_state != SessionState::found || index != 0 ||
      !fixed_argument_matches(api_key, 'B', 128)) {
    return kInvalidValue;
  }
  session_state = SessionState::api_key_set;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetUserPassWordAttempt(const WORD index, BYTE* const current,
                            BYTE* const maximum) {
  const bool before_login = session_state == SessionState::api_key_set;
  const bool after_login = session_state == SessionState::level_read;
  if ((!before_login && !after_login) || index != 0 ||
      current == nullptr || maximum == nullptr) {
    return kInvalidValue;
  }
  *current = 200;
  *maximum = 200;
  session_state = before_login ? SessionState::attempts_read
                               : SessionState::attempts_rechecked;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyUserLogin(const WORD index, char* const user_pin) {
  if (session_state != SessionState::attempts_read || index != 0 ||
      !fixed_argument_matches(user_pin, 'P', 8)) {
    return kInvalidValue;
  }
  session_state = SessionState::logged_in;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetLevel(const WORD index, BYTE* const level) {
  if (session_state != SessionState::logged_in || index != 0 ||
      level == nullptr) {
    return kInvalidValue;
  }
  *level = 1;
  session_state = SessionState::level_read;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyLogoff(const WORD index) {
  if ((session_state != SessionState::attempts_rechecked &&
       session_state != SessionState::restore_verified) ||
      index != 0) {
    return kInvalidValue;
  }
  session_state = SessionState::logged_off;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall VikeyUninitialization() {
  if (session_state != SessionState::logged_off) return kInvalidValue;
  session_state = SessionState::complete;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetHID(const WORD, DWORD* const hardware_id) {
  if (hardware_id != nullptr) *hardware_id = 1;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetUserDataSize(const WORD index, DWORD* const bytes) {
  if (index != 0 || bytes == nullptr) return ERROR_INVALID_PARAMETER;
  switch (session_state) {
    case SessionState::level_read:
      session_state = SessionState::backup_size_read;
      break;
    case SessionState::backup_read:
      session_state = SessionState::test_write_size_read;
      break;
    case SessionState::test_written:
      session_state = SessionState::test_read_size_read;
      break;
    case SessionState::test_verified:
      session_state = SessionState::restore_write_size_read;
      break;
    case SessionState::restored:
      session_state = SessionState::restore_read_size_read;
      break;
    default:
      return kInvalidValue;
  }
  *bytes = 8192;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyReadData(const WORD index, const DWORD address, const DWORD length,
              BYTE* const buffer) {
  if (index != 0 || address != 0 ||
      length != static_cast<DWORD>(user_data.size()) ||
      buffer == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  switch (session_state) {
    case SessionState::backup_size_read:
      session_state = SessionState::backup_read;
      break;
    case SessionState::test_read_size_read:
      if (!std::ranges::all_of(user_data,
                               [](const BYTE value) { return value == 0xa5; })) {
        return kInvalidValue;
      }
      session_state = SessionState::test_verified;
      break;
    case SessionState::restore_read_size_read:
      if (!std::ranges::all_of(user_data,
                               [](const BYTE value) { return value == 0x3c; })) {
        return kInvalidValue;
      }
      session_state = SessionState::restore_verified;
      break;
    default:
      return kInvalidValue;
  }
  std::ranges::copy(user_data, buffer);
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyWriteData(const WORD index, const DWORD address, const DWORD length,
               BYTE* const buffer) {
  if (index != 0 || address != 0 ||
      length != static_cast<DWORD>(user_data.size()) ||
      buffer == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  if (session_state == SessionState::test_write_size_read) {
    if (!std::ranges::all_of(
            std::span(buffer, user_data.size()),
            [](const BYTE value) { return value == 0xa5; })) {
      return kInvalidValue;
    }
    session_state = SessionState::test_written;
  } else if (session_state == SessionState::restore_write_size_read) {
    if (!std::ranges::all_of(
            std::span(buffer, user_data.size()),
            [](const BYTE value) { return value == 0x3c; })) {
      return kInvalidValue;
    }
    session_state = SessionState::restored;
  } else {
    return kInvalidValue;
  }
  std::ranges::copy_n(buffer, user_data.size(), user_data.begin());
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
ViKeyGetModule(const WORD, const WORD, WORD* const value) {
  if (value != nullptr) *value = 1;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
ViKeyDecraseModule(const WORD, const WORD) {
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
ViKeyCheckModule(const WORD, const WORD, WORD* const is_zero,
                 WORD* const can_decrease) {
  if (is_zero != nullptr) *is_zero = 0;
  if (can_decrease != nullptr) *can_decrease = 1;
  return ERROR_SUCCESS;
}
