#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
  initial_data_size_read,
  initial_data_read,
  standalone_written,
  standalone_write_failed,
  test_write_size_read,
  test_written,
  test_read_size_read,
  test_verified,
  restore_write_size_read,
  restored,
  restore_read_size_read,
  restore_verified,
  journal_active,
  logged_off,
  complete,
};

SessionState session_state = SessionState::initial;
std::array<BYTE, 32> user_data = [] {
  std::array<BYTE, 32> result{};
  result.fill(0x3c);
  return result;
}();
constexpr std::size_t kDescriptorSize = 152;
constexpr std::size_t kDescriptorPrefixSize = 120;
constexpr std::size_t kSlot0Offset = 304;
constexpr std::size_t kSlot1Offset = 520;
constexpr std::size_t kRecordSize = 216;
constexpr std::size_t kUserDataSize = 8192;
std::array<BYTE, kUserDataSize> journal_data{};
bool journal_initialized = false;
bool blank_media_session = false;

void store_u32(const std::size_t offset,
               const std::uint32_t value,
               const std::span<BYTE> destination) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    destination[offset + index] =
        static_cast<BYTE>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] bool sha256(const std::span<const BYTE> input,
                          const std::span<BYTE, 32> output) noexcept {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return false;
  }
  const NTSTATUS status = ::BCryptHash(
      algorithm, nullptr, 0,
      const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
      output.data(), static_cast<ULONG>(output.size()));
  static_cast<void>(::BCryptCloseAlgorithmProvider(algorithm, 0));
  return BCRYPT_SUCCESS(status);
}

[[nodiscard]] bool initialize_journal() noexcept {
  if (journal_initialized) return true;
  journal_data.fill(0);
  constexpr std::array<BYTE, 8> kMarkerMagic{
      'U', 'V', 'B', 'L', 'N', 'K', '2', 0};
  std::ranges::copy(kMarkerMagic,
                    journal_data.begin() + kSlot0Offset);
  std::fill_n(journal_data.begin() + kSlot1Offset, kRecordSize,
              static_cast<BYTE>(0xa5));

  std::array<BYTE, kDescriptorSize> descriptor{};
  constexpr std::array<BYTE, 8> kDescriptorMagic{
      'U', 'V', 'J', 'R', 'N', '2', 0, 0};
  std::ranges::copy(kDescriptorMagic, descriptor.begin());
  store_u32(8, 1, descriptor);
  store_u32(12, static_cast<std::uint32_t>(descriptor.size()), descriptor);
  std::fill_n(descriptor.begin() + 16, 16, static_cast<BYTE>(0x11));
  std::fill_n(descriptor.begin() + 32, 16, static_cast<BYTE>(0x22));
  store_u32(48, 0, descriptor);
  store_u32(52, static_cast<std::uint32_t>(kRecordSize), descriptor);
  if (!sha256(std::span(journal_data).subspan(kSlot0Offset, kRecordSize),
              std::span<BYTE, 32>(descriptor.data() + 56, 32)) ||
      !sha256(std::span(journal_data).subspan(kSlot1Offset, kRecordSize),
              std::span<BYTE, 32>(descriptor.data() + 88, 32)) ||
      !sha256(std::span(descriptor).first(kDescriptorPrefixSize),
              std::span<BYTE, 32>(descriptor.data() + 120, 32))) {
    return false;
  }
  std::ranges::copy(descriptor, journal_data.begin());
  std::ranges::copy(descriptor,
                    journal_data.begin() + kDescriptorSize);
  journal_initialized = true;
  return true;
}

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
  const bool product_session = fixed_argument_matches(user_pin, 'P', 8);
  const bool bootstrap_session = fixed_argument_matches(user_pin, 'Q', 8);
  if (session_state != SessionState::attempts_read || index != 0 ||
      (!product_session && !bootstrap_session)) {
    return kInvalidValue;
  }
  blank_media_session = bootstrap_session;
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
  if ((session_state != SessionState::level_read &&
       session_state != SessionState::attempts_rechecked &&
       session_state != SessionState::restore_verified &&
       session_state != SessionState::initial_data_read &&
       session_state != SessionState::standalone_written &&
       session_state != SessionState::standalone_write_failed &&
       session_state != SessionState::journal_active) ||
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
      session_state = SessionState::initial_data_size_read;
      break;
    case SessionState::initial_data_read:
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
    case SessionState::journal_active:
      break;
    default:
      return kInvalidValue;
  }
  *bytes = static_cast<DWORD>(kUserDataSize);
  return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyReadData(const WORD index, const DWORD address, const DWORD length,
              BYTE* const buffer) {
  if (index != 0 || buffer == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  if (session_state == SessionState::initial_data_size_read && address == 0 &&
      length == kDescriptorSize) {
    if (!blank_media_session && !initialize_journal()) return kInvalidValue;
    session_state = SessionState::journal_active;
  }
  if (session_state == SessionState::journal_active) {
    const std::size_t offset = address;
    const std::size_t bytes = length;
    if (offset > journal_data.size() ||
        bytes > journal_data.size() - offset) {
      return ERROR_INVALID_PARAMETER;
    }
    std::ranges::copy(std::span(journal_data).subspan(offset, bytes), buffer);
    return ERROR_SUCCESS;
  }
  if (address != 0 || length != static_cast<DWORD>(user_data.size())) {
    return ERROR_INVALID_PARAMETER;
  }
  switch (session_state) {
    case SessionState::initial_data_size_read:
      session_state = SessionState::initial_data_read;
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
  if (index != 0 || buffer == nullptr) {
    return ERROR_INVALID_PARAMETER;
  }
  if (session_state == SessionState::journal_active) {
    const std::size_t offset = address;
    const std::size_t bytes = length;
    if (offset > journal_data.size() ||
        bytes > journal_data.size() - offset) {
      return ERROR_INVALID_PARAMETER;
    }
    std::ranges::copy_n(buffer, bytes, journal_data.begin() + offset);
    return ERROR_SUCCESS;
  }
  if (address != 0 || length != static_cast<DWORD>(user_data.size())) {
    return ERROR_INVALID_PARAMETER;
  }
  if (session_state == SessionState::test_write_size_read) {
    if (!std::ranges::all_of(
            std::span(buffer, user_data.size()),
            [](const BYTE value) { return value == 0xa5; })) {
      return kInvalidValue;
    }
    session_state = SessionState::test_written;
  } else if (session_state == SessionState::initial_data_size_read) {
    const std::span input(buffer, user_data.size());
    const bool successful_write = std::ranges::all_of(
        input, [](const BYTE value) { return value == 0xd5; });
    const bool partial_failure = std::ranges::all_of(
        input, [](const BYTE value) { return value == 0xe1; });
    if (!successful_write && !partial_failure) return kInvalidValue;
    std::ranges::copy_n(buffer, user_data.size(), user_data.begin());
    session_state = successful_write
                        ? SessionState::standalone_written
                        : SessionState::standalone_write_failed;
    if (partial_failure) return ERROR_WRITE_FAULT;
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
