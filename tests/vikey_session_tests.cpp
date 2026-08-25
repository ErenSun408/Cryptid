#include "card/vikey/vikey_session.h"

#include "usb_vault/card/card_error.h"

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
  sdk::Status logoff_status{sdk::kSuccess};
  sdk::Status uninitialization_status{sdk::kSuccess};
  std::uint32_t device_count{1};
  sdk::Byte current_attempt{200};
  sdk::Byte maximum_attempt{200};
  sdk::Byte level{1};
  bool mutate_arguments{};
  bool credentials_matched{true};
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

sdk::Status __stdcall fake_get_level(const sdk::Index index,
                                     sdk::Byte* const level) {
  FakeSdk& state = fake();
  state.calls.push_back(Call::get_level);
  state.credentials_matched = state.credentials_matched && index == 0;
  if (level != nullptr) *level = state.level;
  return state.get_level_status;
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
      .get_level = &fake_get_level,
      .get_user_password_attempt = &fake_get_attempts,
      .user_login = &fake_user_login,
      .logoff = &fake_logoff,
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
    test_invalid_credentials_fail_before_vendor_calls();
    test_incomplete_functions_fail_before_vendor_calls();
    test_failures_cleanup_without_spurious_logoff();
    test_explicit_close_reports_failure_and_still_uninitializes();
  } catch (const std::exception& error) {
    std::cerr << "vikey_session_tests: FAIL: " << error.what() << '\n';
    return 1;
  }

  std::cout << "vikey_session_tests: PASS\n";
  return 0;
}
