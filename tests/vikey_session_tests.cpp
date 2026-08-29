#include "card/vikey/vikey_blob_round_trip.h"
#include "card/vikey/vikey_session.h"

#include "usb_vault/card/card_error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace sdk = usb_vault::card::vikey::sdk;
using usb_vault::card::CardError;
using usb_vault::card::make_error_code;
using usb_vault::card::vikey::VikeySession;
using usb_vault::card::vikey::verify_development_blob_round_trip;
using usb_vault::security::SecureBuffer;

static_assert(!std::is_copy_constructible_v<VikeySession>);
static_assert(!std::is_move_constructible_v<VikeySession>);

enum class Call {
  set_user_sn,
  find_ex,
  set_api_key,
  get_attempts,
  user_login,
  get_level,
  get_user_data_size,
  read_data,
  write_data,
  logoff,
  uninitialization,
};

struct FakeSdk final {
  sdk::Status set_user_sn_status{sdk::kSuccess};
  sdk::Status find_status{sdk::kSuccess};
  sdk::Status set_api_key_status{sdk::kSuccess};
  sdk::Status get_attempts_status{sdk::kSuccess};
  sdk::Status login_status{sdk::kSuccess};
  sdk::Status get_level_status{sdk::kSuccess};
  sdk::Status get_hid_status{sdk::kSuccess};
  std::vector<sdk::Status> get_hid_statuses;
  sdk::Status get_user_data_size_status{sdk::kSuccess};
  sdk::Status read_data_status{sdk::kSuccess};
  sdk::Status write_data_status{sdk::kSuccess};
  std::vector<sdk::Status> read_data_statuses;
  std::vector<sdk::Status> write_data_statuses;
  sdk::Status logoff_status{sdk::kSuccess};
  sdk::Status uninitialization_status{sdk::kSuccess};
  std::uint32_t device_count{1};
  std::uint32_t hardware_id{0x12345678U};
  std::vector<std::uint32_t> hardware_ids;
  sdk::Byte current_attempt{200};
  sdk::Byte maximum_attempt{200};
  sdk::Byte level{1};
  sdk::Length user_data_size{8192};
  std::array<std::byte, usb_vault::card::vikey::kVikeyBlobBytes> user_data{};
  std::size_t corrupt_read_call{static_cast<std::size_t>(-1)};
  bool mutate_arguments{};
  bool credentials_matched{true};
  std::size_t read_data_calls{};
  std::size_t write_data_calls{};
  std::size_t get_hid_calls{};
  std::vector<Call> calls;
};

FakeSdk* active_fake = nullptr;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
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

FakeSdk& fake() {
  if (active_fake == nullptr) throw std::runtime_error("no active fake SDK");
  return *active_fake;
}

[[nodiscard]] sdk::Status status_for_call(
    const std::vector<sdk::Status>& statuses,
    const std::size_t call, const sdk::Status fallback) noexcept {
  return call < statuses.size() ? statuses[call] : fallback;
}

sdk::Status __stdcall fake_set_user_sn(char* const value) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::set_user_sn);
  state.credentials_matched =
      state.credentials_matched &&
      fixed_argument_matches(
          value, 'A', usb_vault::card::vikey::kVikeyUserSnBytes);
  if (state.mutate_arguments && value != nullptr) value[0] = '0';
  return state.set_user_sn_status;
}

sdk::Status __stdcall fake_find_ex(std::uint32_t* const count) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::find_ex);
  if (count != nullptr) *count = state.device_count;
  return state.find_status;
}

sdk::Status __stdcall fake_set_api_key(const sdk::Index index,
                                       char* const value) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::set_api_key);
  state.credentials_matched =
      state.credentials_matched && index == 0 &&
      fixed_argument_matches(
          value, 'B', usb_vault::card::vikey::kVikeyApiKeyBytes);
  if (state.mutate_arguments && value != nullptr) value[0] = '0';
  return state.set_api_key_status;
}

sdk::Status __stdcall fake_uninitialization() {
  FakeSdk& state = fake();
  state.calls.push_back(Call::uninitialization);
  return state.uninitialization_status;
}

sdk::Status __stdcall fake_get_hid(
    const sdk::Index index, std::uint32_t* const hardware_id) {
  FakeSdk& state = fake();
  const std::size_t call = state.get_hid_calls++;
  state.credentials_matched = state.credentials_matched && index == 0;
  const sdk::Status status = status_for_call(
      state.get_hid_statuses, call, state.get_hid_status);
  if (status == sdk::kSuccess && hardware_id != nullptr) {
    *hardware_id = call < state.hardware_ids.size()
                       ? state.hardware_ids[call]
                       : state.hardware_id;
  }
  return status;
}

sdk::Status __stdcall fake_get_level(const sdk::Index index,
                                     sdk::Byte* const level) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::get_level);
  state.credentials_matched = state.credentials_matched && index == 0;
  if (level != nullptr) *level = state.level;
  return state.get_level_status;
}

sdk::Status __stdcall fake_get_user_data_size(
    const sdk::Index index, sdk::Length* const bytes) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::get_user_data_size);
  state.credentials_matched = state.credentials_matched && index == 0;
  if (bytes != nullptr) *bytes = state.user_data_size;
  return state.get_user_data_size_status;
}

sdk::Status __stdcall fake_read_data(
    const sdk::Index index, const sdk::Address address,
    const sdk::Length length, sdk::Byte* const buffer) {
  FakeSdk& state = fake();
  const std::size_t call = state.read_data_calls++;
  state.calls.push_back(Call::read_data);
  state.credentials_matched =
      state.credentials_matched && index == 0 &&
      address == usb_vault::card::vikey::kVikeyBlobAddress &&
      length == usb_vault::card::vikey::kVikeyBlobBytes && buffer != nullptr;
  if (buffer != nullptr && length == state.user_data.size()) {
    for (std::size_t offset = 0; offset < state.user_data.size(); ++offset) {
      buffer[offset] = std::to_integer<sdk::Byte>(state.user_data[offset]);
    }
    if (call == state.corrupt_read_call) buffer[0] ^= 1U;
  }
  return status_for_call(state.read_data_statuses, call,
                         state.read_data_status);
}

sdk::Status __stdcall fake_write_data(
    const sdk::Index index, const sdk::Address address,
    const sdk::Length length, sdk::Byte* const buffer) {
  FakeSdk& state = fake();
  const std::size_t call = state.write_data_calls++;
  state.calls.push_back(Call::write_data);
  state.credentials_matched =
      state.credentials_matched && index == 0 &&
      address == usb_vault::card::vikey::kVikeyBlobAddress &&
      length == usb_vault::card::vikey::kVikeyBlobBytes && buffer != nullptr;
  if (buffer != nullptr && length == state.user_data.size()) {
    for (std::size_t offset = 0; offset < state.user_data.size(); ++offset) {
      state.user_data[offset] = static_cast<std::byte>(buffer[offset]);
    }
    if (state.mutate_arguments) buffer[0] = 0;
  }
  return status_for_call(state.write_data_statuses, call,
                         state.write_data_status);
}

sdk::Status __stdcall fake_get_attempts(const sdk::Index index,
                                        sdk::Byte* const current,
                                        sdk::Byte* const maximum) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::get_attempts);
  state.credentials_matched = state.credentials_matched && index == 0;
  if (current != nullptr) *current = state.current_attempt;
  if (maximum != nullptr) *maximum = state.maximum_attempt;
  return state.get_attempts_status;
}

sdk::Status __stdcall fake_user_login(const sdk::Index index,
                                      char* const value) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::user_login);
  state.credentials_matched =
      state.credentials_matched && index == 0 &&
      fixed_argument_matches(
          value, 'P', usb_vault::card::vikey::kVikeyUserPinBytes);
  if (state.mutate_arguments && value != nullptr) value[0] = '0';
  return state.login_status;
}

sdk::Status __stdcall fake_logoff(const sdk::Index index) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::logoff);
  state.credentials_matched = state.credentials_matched && index == 0;
  return state.logoff_status;
}

[[nodiscard]] sdk::Functions fake_functions() noexcept {
  return {
      .set_user_sn = &fake_set_user_sn,
      .find_ex = &fake_find_ex,
      .set_api_key = &fake_set_api_key,
      .uninitialization = &fake_uninitialization,
      .get_hid = &fake_get_hid,
      .get_level = &fake_get_level,
      .get_user_data_size = &fake_get_user_data_size,
      .get_user_password_attempt = &fake_get_attempts,
      .user_login = &fake_user_login,
      .logoff = &fake_logoff,
      .read_data = &fake_read_data,
      .write_data = &fake_write_data,
  };
}

class ActiveFake final {
 public:
  explicit ActiveFake(FakeSdk& state) {
    require(active_fake == nullptr, "nested fake SDK activation");
    active_fake = &state;
  }
  ~ActiveFake() noexcept { active_fake = nullptr; }

  ActiveFake(const ActiveFake&) = delete;
  ActiveFake& operator=(const ActiveFake&) = delete;
};

struct Credentials final {
  Credentials()
      : user_sn(usb_vault::card::vikey::kVikeyUserSnBytes),
        api_key(usb_vault::card::vikey::kVikeyApiKeyBytes),
        user_pin(usb_vault::card::vikey::kVikeyUserPinBytes) {
    for (std::byte& value : user_sn.bytes()) value = std::byte{'A'};
    for (std::byte& value : api_key.bytes()) value = std::byte{'B'};
    for (std::byte& value : user_pin.bytes()) value = std::byte{'P'};
  }

  SecureBuffer user_sn;
  SecureBuffer api_key;
  SecureBuffer user_pin;
};

template <typename Operation>
void require_card_error(Operation&& operation, const CardError expected,
                        const char* const message) {
  try {
    std::forward<Operation>(operation)();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

void require_calls(const FakeSdk& state, const std::vector<Call>& expected,
                   const char* const message) {
  require(state.calls == expected, message);
}

[[nodiscard]] bool all_bytes_equal(const SecureBuffer& buffer,
                                   const std::byte expected) noexcept {
  for (const std::byte value : buffer.bytes()) {
    if (value != expected) return false;
  }
  return true;
}

void test_successful_session_has_exact_lifecycle() {
  FakeSdk state{.mutate_arguments = true};
  ActiveFake activation(state);
  Credentials credentials;
  VikeySession session = VikeySession::open(
      fake_functions(), credentials.user_sn, credentials.api_key,
      credentials.user_pin);
  require(session.active(), "successful ViKey session was not active");
  require(session.index() == 0, "ViKey session selected the wrong index");
  require(session.hardware_id() == state.hardware_id &&
              state.get_hid_calls == 2,
          "ViKey session did not bind the stable hardware identity");
  require(session.user_pin_attempts_remaining() == 200 &&
              session.user_pin_attempts_maximum() == 200,
          "ViKey session did not retain the observed PIN attempt budget");
  require(state.credentials_matched,
          "ViKey session changed or unterminated a credential");
  require(all_bytes_equal(credentials.user_sn, std::byte{'A'}) &&
              all_bytes_equal(credentials.api_key, std::byte{'B'}) &&
              all_bytes_equal(credentials.user_pin, std::byte{'P'}),
          "vendor mutation escaped the temporary credential copies");
  require_calls(state,
                {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                 Call::get_attempts, Call::user_login, Call::get_level},
                "ViKey session initialization order differed");

  require(!session.close(), "successful ViKey close returned an error");
  require(!session.active(), "closed ViKey session remained active");
  require_calls(state,
                {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                 Call::get_attempts, Call::user_login, Call::get_level,
                 Call::logoff, Call::uninitialization},
                "ViKey session cleanup order differed");
  require(!session.close(), "second ViKey close was not idempotent");
  require(state.calls.size() == 8,
          "second ViKey close repeated vendor cleanup");
}

void test_destructor_closes_active_session() {
  FakeSdk state;
  ActiveFake activation(state);
  Credentials credentials;
  {
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require(session.active(), "destructor test session was not active");
  }
  require_calls(state,
                {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                 Call::get_attempts, Call::user_login, Call::get_level,
                 Call::logoff, Call::uninitialization},
                "ViKey destructor did not complete cleanup");
}

void test_fixed_user_blob_read_and_write() {
  FakeSdk state{.mutate_arguments = true};
  for (std::size_t offset = 0; offset < state.user_data.size(); ++offset) {
    state.user_data[offset] = static_cast<std::byte>(offset);
  }
  ActiveFake activation(state);
  Credentials credentials;
  VikeySession session = VikeySession::open(
      fake_functions(), credentials.user_sn, credentials.api_key,
      credentials.user_pin);

  SecureBuffer read = session.read_user_blob();
  require(read.size() == usb_vault::card::vikey::kVikeyBlobBytes,
          "ViKey user blob read returned the wrong size");
  for (std::size_t offset = 0; offset < read.size(); ++offset) {
    require(read.bytes()[offset] == static_cast<std::byte>(offset),
            "ViKey user blob read returned the wrong data");
  }

  SecureBuffer replacement(usb_vault::card::vikey::kVikeyBlobBytes);
  for (std::byte& value : replacement.bytes()) value = std::byte{0xa5};
  session.write_user_blob(replacement);
  require(all_bytes_equal(replacement, std::byte{0xa5}),
          "vendor mutation escaped the temporary blob copy");
  for (const std::byte value : state.user_data) {
    require(value == std::byte{0xa5},
            "ViKey user blob write stored the wrong data");
  }
  require_calls(
      state,
      {Call::set_user_sn, Call::find_ex, Call::set_api_key,
       Call::get_attempts, Call::user_login, Call::get_level,
       Call::get_user_data_size, Call::read_data,
       Call::get_user_data_size, Call::write_data},
      "ViKey fixed blob operation order differed");
  require(!session.close(), "blob test session did not close");
}

void test_fixed_user_blob_failures() {
  Credentials credentials;

  {
    FakeSdk state{.get_user_data_size_status = sdk::kCommunication};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::communication_failed,
                       "ViKey capacity failure mapped to the wrong error");
    require(state.calls.back() == Call::get_user_data_size,
            "capacity failure reached the read operation");
    require(!session.close(), "capacity-failure session did not close");
  }

  {
    FakeSdk state{.user_data_size =
                      usb_vault::card::vikey::kVikeyBlobBytes - 1};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::invalid_configuration,
                       "undersized ViKey user area was accepted");
    require(state.calls.back() == Call::get_user_data_size,
            "undersized user area reached the read operation");
    require(!session.close(), "undersized-area session did not close");
  }

  {
    FakeSdk state{.read_data_status = sdk::kCommunication};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::communication_failed,
                       "ViKey blob read failure mapped to the wrong error");
    require(!session.close(), "read-failure session did not close");
  }

  {
    FakeSdk state{.write_data_status = sdk::kInvalidAddressOrSize};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer blob(usb_vault::card::vikey::kVikeyBlobBytes);
    require_card_error([&] { session.write_user_blob(blob); },
                       CardError::invalid_operation,
                       "ViKey blob write failure mapped to the wrong error");
    require(!session.close(), "write-failure session did not close");
  }

  {
    FakeSdk state;
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer short_blob(usb_vault::card::vikey::kVikeyBlobBytes - 1);
    const std::size_t calls_before = state.calls.size();
    require_card_error([&] { session.write_user_blob(short_blob); },
                       CardError::invalid_configuration,
                       "short ViKey user blob was accepted");
    require(state.calls.size() == calls_before,
            "short user blob reached the vendor SDK");
    require(!session.close(), "short-blob session did not close");
    const std::size_t calls_after_close = state.calls.size();
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::invalid_operation,
                       "closed ViKey session allowed a blob read");
    require(state.calls.size() == calls_after_close,
            "closed-session read reached the vendor SDK");
  }
}

[[nodiscard]] SecureBuffer fixed_blob(const std::byte value) {
  SecureBuffer blob(usb_vault::card::vikey::kVikeyBlobBytes);
  for (std::byte& byte : blob.bytes()) byte = value;
  return blob;
}

void fill_user_data(FakeSdk& state, const std::byte value) noexcept {
  for (std::byte& byte : state.user_data) byte = value;
}

void require_user_data(const FakeSdk& state, const std::byte expected,
                       const char* const message) {
  for (const std::byte byte : state.user_data) {
    if (byte != expected) throw std::runtime_error(message);
  }
}

void test_development_blob_round_trip_restores_original() {
  FakeSdk state;
  fill_user_data(state, std::byte{0x3c});
  ActiveFake activation(state);
  Credentials credentials;
  VikeySession session = VikeySession::open(
      fake_functions(), credentials.user_sn, credentials.api_key,
      credentials.user_pin);
  SecureBuffer test_blob = fixed_blob(std::byte{0xa5});

  verify_development_blob_round_trip(session, test_blob);

  require_user_data(state, std::byte{0x3c},
                    "successful round trip did not restore original data");
  require(all_bytes_equal(test_blob, std::byte{0xa5}),
          "round trip changed the caller test blob");
  require(state.read_data_calls == 3 && state.write_data_calls == 2,
          "successful round trip used the wrong read/write count");
  require(!session.close(), "successful round-trip session did not close");
}

void test_development_blob_round_trip_restores_after_test_failures() {
  Credentials credentials;

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.write_data_statuses = {sdk::kCommunication, sdk::kSuccess};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::communication_failed,
        "partial test write failure returned the wrong error");
    require_user_data(state, std::byte{0x3c},
                      "partial test write failure was not restored");
    require(state.write_data_calls == 2 && state.read_data_calls == 2,
            "partial test write failure skipped verified restoration");
    require(!session.close(), "partial-write session did not close");
  }

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.read_data_statuses = {sdk::kSuccess, sdk::kCommunication,
                                sdk::kSuccess};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::communication_failed,
        "test verification read failure returned the wrong error");
    require_user_data(state, std::byte{0x3c},
                      "test verification read failure was not restored");
    require(state.write_data_calls == 2 && state.read_data_calls == 3,
            "test read failure skipped verified restoration");
    require(!session.close(), "test-read-failure session did not close");
  }

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.corrupt_read_call = 1;
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::device_failure,
        "test blob mismatch returned the wrong error");
    require_user_data(state, std::byte{0x3c},
                      "test blob mismatch was not restored");
    require(state.write_data_calls == 2 && state.read_data_calls == 3,
            "test blob mismatch skipped verified restoration");
    require(!session.close(), "test-mismatch session did not close");
  }
}

void test_development_blob_round_trip_reports_unverified_restore() {
  Credentials credentials;

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.write_data_statuses = {sdk::kSuccess, sdk::kCommunication};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::device_failure,
        "failed restore write was not promoted to device failure");
    require(state.write_data_calls == 2 && state.read_data_calls == 2,
            "failed restore write performed an invalid verification read");
    require(!session.close(), "restore-write-failure session did not close");
  }

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.read_data_statuses = {sdk::kSuccess, sdk::kSuccess,
                                sdk::kCommunication};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::device_failure,
        "failed restore verification was not promoted to device failure");
    require(state.write_data_calls == 2 && state.read_data_calls == 3,
            "restore verification failure used the wrong operation count");
    require(!session.close(), "restore-read-failure session did not close");
  }

  {
    FakeSdk state;
    fill_user_data(state, std::byte{0x3c});
    state.read_data_statuses = {sdk::kCommunication};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer test_blob = fixed_blob(std::byte{0xa5});
    require_card_error(
        [&] { verify_development_blob_round_trip(session, test_blob); },
        CardError::communication_failed,
        "backup read failure returned the wrong error");
    require(state.write_data_calls == 0 && state.read_data_calls == 1,
            "backup read failure attempted a write without an original");
    require_user_data(state, std::byte{0x3c},
                      "backup read failure changed user data");
    require(!session.close(), "backup-read-failure session did not close");
  }

  {
    FakeSdk state;
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    SecureBuffer short_blob(usb_vault::card::vikey::kVikeyBlobBytes - 1);
    const std::size_t calls_before = state.calls.size();
    require_card_error(
        [&] { verify_development_blob_round_trip(session, short_blob); },
        CardError::invalid_configuration,
        "short development test blob was accepted");
    require(state.calls.size() == calls_before,
            "short development test blob reached the vendor SDK");
    require(!session.close(), "short-test-blob session did not close");
  }
}

void test_invalid_credentials_fail_before_vendor_calls() {
  FakeSdk state;
  ActiveFake activation(state);
  Credentials credentials;
  SecureBuffer short_user_sn(
      usb_vault::card::vikey::kVikeyUserSnBytes - 1);
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            fake_functions(), short_user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "short ViKey UserSN was not rejected");
  require(state.calls.empty(), "invalid UserSN reached the vendor SDK");

  credentials.api_key.bytes()[3] = std::byte{'G'};
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            fake_functions(), credentials.user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "non-hex ViKey API key was not rejected");
  require(state.calls.empty(), "invalid API key reached the vendor SDK");

  credentials.api_key.bytes()[3] = std::byte{'B'};
  credentials.user_pin.bytes()[2] = std::byte{'-'};
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            fake_functions(), credentials.user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "non-alphanumeric ViKey PIN was not rejected");
  require(state.calls.empty(), "invalid PIN reached the vendor SDK");
}

void test_incomplete_functions_fail_before_vendor_calls() {
  FakeSdk state;
  ActiveFake activation(state);
  Credentials credentials;
  sdk::Functions functions = fake_functions();
  functions.logoff = nullptr;
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            functions, credentials.user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "incomplete session functions were not rejected");
  require(state.calls.empty(), "incomplete functions reached the vendor SDK");

  functions = fake_functions();
  functions.read_data = nullptr;
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            functions, credentials.user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "missing ViKey data function was not rejected");
  require(state.calls.empty(),
          "missing data function reached the vendor SDK");

  functions = fake_functions();
  functions.get_hid = nullptr;
  require_card_error(
      [&] {
        static_cast<void>(VikeySession::open(
            functions, credentials.user_sn, credentials.api_key,
            credentials.user_pin));
      },
      CardError::invalid_configuration,
      "missing ViKey identity function was not rejected");
  require(state.calls.empty() && state.get_hid_calls == 0,
          "missing identity function reached the vendor SDK");
}

void test_hardware_identity_failures_fail_closed() {
  Credentials credentials;

  {
    FakeSdk state{.get_hid_status = sdk::kCommunication};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::communication_failed,
        "initial ViKey identity failure mapped to the wrong error");
    require(state.get_hid_calls == 1,
            "initial identity failure used the wrong query count");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex,
                   Call::uninitialization},
                  "initial identity failure cleanup differed");
  }

  {
    FakeSdk state{.hardware_id = 0};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::device_failure,
        "zero ViKey hardware identity was accepted");
    require(state.get_hid_calls == 1,
            "zero identity used the wrong query count");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex,
                   Call::uninitialization},
                  "zero identity cleanup differed");
  }

  {
    FakeSdk state{
        .hardware_ids = {0x11111111U, 0x22222222U}};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::device_failure,
        "identity change during authentication was accepted");
    require(state.get_hid_calls == 2,
            "authentication identity change used the wrong query count");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::user_login, Call::get_level,
                   Call::logoff, Call::uninitialization},
                  "authentication identity change cleanup differed");
  }

  {
    FakeSdk state{
        .hardware_ids = {0x11111111U, 0x11111111U, 0x22222222U}};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::device_failure,
                       "active-session card replacement was accepted");
    require(state.get_hid_calls == 3 && state.read_data_calls == 0,
            "card replacement reached the user data operation");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::user_login, Call::get_level},
                  "card replacement reached the capacity query");
    require(!session.close(), "replacement-detection session did not close");
  }

  {
    FakeSdk state{
        .get_hid_statuses = {sdk::kSuccess, sdk::kSuccess,
                             sdk::kCommunication}};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require_card_error([&] { static_cast<void>(session.read_user_blob()); },
                       CardError::communication_failed,
                       "active-session identity failure mapped incorrectly");
    require(state.get_hid_calls == 3 && state.read_data_calls == 0,
            "identity communication failure reached user data");
    require(!session.close(), "identity-failure session did not close");
  }
}

void test_failures_cleanup_without_spurious_logoff() {
  Credentials credentials;

  {
    FakeSdk state{.set_user_sn_status = sdk::kUserSn};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::invalid_configuration,
        "UserSN failure mapped to the wrong error");
    require_calls(state, {Call::set_user_sn, Call::uninitialization},
                  "UserSN failure cleanup differed");
  }

  {
    FakeSdk state{.device_count = 0};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::device_not_found,
        "zero ViKey devices mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex,
                   Call::uninitialization},
                  "zero-device cleanup differed");
  }

  {
    FakeSdk state{.device_count = 2};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::invalid_operation,
        "multiple ViKey devices were not rejected");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex,
                   Call::uninitialization},
                  "multiple-device cleanup differed");
  }

  {
    FakeSdk state{.set_api_key_status = sdk::kApiKeyCalculate1};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::invalid_configuration,
        "API-key calculation failure mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::uninitialization},
                  "API-key failure cleanup differed");
  }

  {
    FakeSdk state{.get_attempts_status = sdk::kCommunication};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::communication_failed,
        "PIN-attempt query failure mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::uninitialization},
                  "PIN-attempt query failure cleanup differed");
  }

  {
    FakeSdk state{.current_attempt = 0};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::authentication_locked,
        "exhausted PIN attempts did not fail before login");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::uninitialization},
                  "locked-PIN cleanup differed");
  }

  {
    FakeSdk state{.current_attempt = 201, .maximum_attempt = 200};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::device_failure,
        "inconsistent PIN attempts mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::uninitialization},
                  "inconsistent PIN-attempt cleanup differed");
  }

  {
    FakeSdk state{.current_attempt = 0, .maximum_attempt = 0};
    ActiveFake activation(state);
    VikeySession session = VikeySession::open(
        fake_functions(), credentials.user_sn, credentials.api_key,
        credentials.user_pin);
    require(session.active(),
            "unlimited PIN-attempt policy was treated as locked");
    require(!session.close(), "unlimited-attempt session did not close");
  }

  {
    FakeSdk state{.login_status = sdk::kInvalidPassword};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::authentication_failed,
        "invalid PIN mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::user_login,
                   Call::uninitialization},
                  "failed-login cleanup issued a logoff");
  }

  {
    FakeSdk state{.get_level_status = sdk::kCommunication};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::communication_failed,
        "post-login level failure mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::user_login, Call::get_level,
                   Call::logoff, Call::uninitialization},
                  "post-login level failure did not log off");
  }

  {
    FakeSdk state{.level = 0};
    ActiveFake activation(state);
    require_card_error(
        [&] {
          static_cast<void>(VikeySession::open(
              fake_functions(), credentials.user_sn, credentials.api_key,
              credentials.user_pin));
        },
        CardError::device_failure,
        "wrong post-login level mapped to the wrong error");
    require_calls(state,
                  {Call::set_user_sn, Call::find_ex, Call::set_api_key,
                   Call::get_attempts, Call::user_login, Call::get_level,
                   Call::logoff, Call::uninitialization},
                  "post-login failure did not log off");
  }
}

void test_explicit_close_reports_failure_and_still_uninitializes() {
  FakeSdk state;
  ActiveFake activation(state);
  Credentials credentials;
  VikeySession session = VikeySession::open(
      fake_functions(), credentials.user_sn, credentials.api_key,
      credentials.user_pin);
  state.logoff_status = sdk::kCommunication;
  const std::error_code error = session.close();
  require(error == make_error_code(CardError::communication_failed),
          "logoff failure mapped to the wrong error");
  require(state.calls[state.calls.size() - 2] == Call::logoff &&
              state.calls.back() == Call::uninitialization,
          "uninitialization was skipped after logoff failure");
  require(!session.active(), "failed explicit close retained session state");
}

}  // namespace

int main() {
  try {
    test_successful_session_has_exact_lifecycle();
    test_destructor_closes_active_session();
    test_fixed_user_blob_read_and_write();
    test_fixed_user_blob_failures();
    test_development_blob_round_trip_restores_original();
    test_development_blob_round_trip_restores_after_test_failures();
    test_development_blob_round_trip_reports_unverified_restore();
    test_invalid_credentials_fail_before_vendor_calls();
    test_incomplete_functions_fail_before_vendor_calls();
    test_hardware_identity_failures_fail_closed();
    test_failures_cleanup_without_spurious_logoff();
    test_explicit_close_reports_failure_and_still_uninitializes();
  } catch (const std::exception& error) {
    std::cerr << "vikey_session_tests: FAIL: " << error.what() << '\n';
    return 1;
  }

  std::cout << "vikey_session_tests: PASS\n";
  return 0;
}
