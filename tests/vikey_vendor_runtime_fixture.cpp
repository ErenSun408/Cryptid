#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <iterator>

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
  logged_off,
  complete,
};

SessionState session_state = SessionState::initial;

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
  if (session_state != SessionState::api_key_set || index != 0 ||
      current == nullptr || maximum == nullptr) {
    return kInvalidValue;
  }
  *current = 200;
  *maximum = 200;
  session_state = SessionState::attempts_read;
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
  if (session_state != SessionState::level_read || index != 0) {
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
VikeyGetUserDataSize(const WORD, DWORD* const bytes) {
  if (bytes != nullptr) *bytes = 8192;
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyReadData(const WORD, const DWORD, const DWORD, BYTE*) {
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyWriteData(const WORD, const DWORD, const DWORD, BYTE*) {
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
