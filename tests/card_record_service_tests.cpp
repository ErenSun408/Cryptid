#include "usb_vault/card/card_record_service.h"

#include "usb_vault/card/card_error.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::card::CardCapabilities;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordService;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardStore;
using usb_vault::card::UnexpectedCardRecordState;
using usb_vault::card::kCardRecordV1RootSecretSize;
using usb_vault::card::kCardRecordV1Size;
using usb_vault::card::make_error_code;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::security::SecureBuffer;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

[[nodiscard]] std::vector<std::byte> copy_bytes(
    const std::span<const std::byte> source) {
  return {source.begin(), source.end()};
}

enum class WriteBehavior {
  success,
  write_then_throw,
  throw_without_write,
  corrupt_after_write,
  fail_readback,
};

class FakeCardStore final : public ICardStore {
 public:
  explicit FakeCardStore(const std::span<const std::byte> initial)
      : blob(initial.begin(), initial.end()) {}

  [[nodiscard]] SecureBuffer get_blob() override {
    ++get_calls;
    if (get_calls == 2 && !second_read_blob.empty()) {
      blob = second_read_blob;
    }
    if (fail_next_read) {
      fail_next_read = false;
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
    SecureBuffer result(blob.size());
    std::ranges::copy(blob, result.bytes().begin());
    return result;
  }

  void put_blob(const SecureBuffer& replacement) override {
    ++put_calls;
    if (behavior == WriteBehavior::throw_without_write) {
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
    blob.assign(replacement.bytes().begin(), replacement.bytes().end());
    if (behavior == WriteBehavior::corrupt_after_write) {
      blob.back() ^= std::byte{1};
    }
    if (behavior == WriteBehavior::fail_readback) {
      fail_next_read = true;
    }
    if (behavior == WriteBehavior::write_then_throw) {
      throw std::system_error(make_error_code(CardError::communication_failed));
    }
  }

  [[nodiscard]] CardCapabilities capabilities() const noexcept override {
    return {};
  }

  std::vector<std::byte> blob;
  std::vector<std::byte> second_read_blob;
  WriteBehavior behavior{WriteBehavior::success};
  bool fail_next_read{false};
  std::size_t get_calls{};
  std::size_t put_calls{};
};

template <typename Action>
void require_card_error(Action&& action, const CardError expected,
                        const char* const message) {
  try {
    action();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

template <typename Action>
void require_state_error(Action&& action, const CardRecordState expected,
                         const char* const message) {
  try {
    action();
  } catch (const UnexpectedCardRecordState& error) {
    require(error.observed() == expected, message);
    return;
  }
  throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer marker() {
  return provision_uninitialized_card_record_v1();
}

void store_u32(std::vector<std::byte>& destination,
               const std::size_t offset,
               const std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    destination[offset + index] =
        static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

[[nodiscard]] std::vector<std::byte> future_record() {
  std::vector<std::byte> result(kCardRecordV1Size);
  constexpr std::array<std::byte, 8> kMagic{
      std::byte{'U'}, std::byte{'V'}, std::byte{'C'}, std::byte{'R'},
      std::byte{'D'}, std::byte{'2'}, std::byte{},    std::byte{}};
  std::ranges::copy(kMagic, result.begin());
  store_u32(result, 8, 2);
  store_u32(result, 12, static_cast<std::uint32_t>(result.size()));
  return result;
}

void test_public_states_are_reported_without_password_work() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);
  require(service.inspect() == CardRecordState::provisioned_uninitialized,
          "provisioning state was not reported");

  store.blob.assign(32, std::byte{0x5a});
  require(service.inspect() == CardRecordState::legacy_p1_blob,
          "legacy state was not reported");

  store.blob = future_record();
  require(service.inspect() == CardRecordState::unsupported_version,
          "unsupported state was not reported");

  store.blob.assign(kCardRecordV1Size, std::byte{});
  require(service.inspect() == CardRecordState::malformed,
          "malformed state was not reported");
  require(store.put_calls == 0, "state inspection wrote the card");
}

void test_initialization_enters_authenticated_state() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);

  SecureBuffer initialized_root = service.initialize_and_authenticate(
      secure_text("service-daily-password-v1"));
  require(initialized_root.size() == kCardRecordV1RootSecretSize,
          "initialization returned the wrong root size");
  require(store.get_calls == 3 && store.put_calls == 1,
          "initialization did not recheck before one write and readback");
  require(service.inspect() == CardRecordState::initialized_v1,
          "initialization did not persist an initialized record");

  SecureBuffer login_root =
      service.authenticate(secure_text("service-daily-password-v1"));
  require(std::ranges::equal(initialized_root.bytes(), login_root.bytes()),
          "initialization and login returned different roots");
}

void test_second_password_opens_the_same_record() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);

  SecureBuffer root = service.initialize_and_authenticate(
      secure_text("service-daily-password-v1"));
  const std::vector<std::byte> initialized = copy_bytes(store.blob);

  service.set_second_password(secure_text("service-daily-password-v1"),
                              secure_text("service-second-password-v1"));
  require(store.blob.size() == kCardRecordV1Size,
          "the rewritten record changed size");
  require(store.blob != initialized,
          "the rewritten record is byte-identical to the previous one");
  require(service.inspect() == CardRecordState::initialized_v1,
          "the rewritten record left the initialized state");

  SecureBuffer existing_root =
      service.authenticate(secure_text("service-daily-password-v1"));
  SecureBuffer second_root =
      service.authenticate(secure_text("service-second-password-v1"));
  require(std::ranges::equal(root.bytes(), existing_root.bytes()) &&
              std::ranges::equal(root.bytes(), second_root.bytes()),
          "the two passwords no longer open the same root");
}

void test_second_password_requires_an_initialized_record() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);
  const std::vector<std::byte> untouched = copy_bytes(store.blob);

  require_state_error(
      [&] {
        service.set_second_password(secure_text("service-daily-password-v1"),
                                    secure_text("service-second-password-v1"));
      },
      CardRecordState::provisioned_uninitialized,
      "a provisioning record accepted a second password");
  require(store.blob == untouched && store.put_calls == 0,
          "a rejected second password still wrote the card");

  SecureBuffer root = service.initialize_and_authenticate(
      secure_text("service-daily-password-v1"));
  const std::vector<std::byte> initialized = copy_bytes(store.blob);
  for (auto [existing, second] : {
           std::pair<std::string_view, std::string_view>{
               "", "service-second-password-v1"},
           std::pair<std::string_view, std::string_view>{
               "service-daily-password-v1", ""}}) {
    bool rejected = false;
    try {
      service.set_second_password(secure_text(existing), secure_text(second));
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "an empty card record password was accepted");
  }
  require(store.blob == initialized,
          "a rejected second password still changed the record");
}

void test_second_password_write_failure_is_verified_against_the_card() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);
  SecureBuffer root = service.initialize_and_authenticate(
      secure_text("service-daily-password-v1"));

  store.behavior = WriteBehavior::write_then_throw;
  service.set_second_password(secure_text("service-daily-password-v1"),
                              secure_text("service-second-password-v1"));
  SecureBuffer second_root =
      service.authenticate(secure_text("service-second-password-v1"));
  require(std::ranges::equal(root.bytes(), second_root.bytes()),
          "a verified complete write was not accepted");

  store.behavior = WriteBehavior::throw_without_write;
  const std::vector<std::byte> before = copy_bytes(store.blob);
  bool reported = false;
  try {
    service.set_second_password(secure_text("service-second-password-v1"),
                                secure_text("service-third-password-v1"));
  } catch (const std::exception&) {
    reported = true;
  }
  require(reported && store.blob == before,
          "a failed write was hidden or changed the record");
}

void test_non_provisioning_states_never_initialize() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  CardRecordService service(store);

  store.blob.assign(32, std::byte{0x41});
  require_state_error(
      [&] {
        static_cast<void>(service.initialize_and_authenticate(
            secure_text("must-not-run-kdf-v1")));
      },
      CardRecordState::legacy_p1_blob,
      "legacy record reached initialization");

  store.blob = future_record();
  require_state_error(
      [&] {
        static_cast<void>(service.initialize_and_authenticate(
            secure_text("must-not-run-kdf-v2")));
      },
      CardRecordState::unsupported_version,
      "unsupported record reached initialization");

  store.blob.assign(kCardRecordV1Size, std::byte{});
  require_state_error(
      [&] {
        static_cast<void>(service.initialize_and_authenticate(
            secure_text("must-not-run-kdf-v3")));
      },
      CardRecordState::malformed,
      "malformed record reached initialization");
  require(store.put_calls == 0,
          "non-provisioning state wrote the card");

  SecureBuffer marker_value = marker();
  store.blob = copy_bytes(marker_value.bytes());
  require_state_error(
      [&] {
        static_cast<void>(
            service.authenticate(secure_text("must-not-run-kdf-v4")));
      },
      CardRecordState::provisioned_uninitialized,
      "provisioning record reached authentication");
}

void test_verified_complete_write_error_is_accepted() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  store.behavior = WriteBehavior::write_then_throw;
  CardRecordService service(store);

  SecureBuffer root = service.initialize_and_authenticate(
      secure_text("service-daily-password-v2"));
  require(root.size() == kCardRecordV1RootSecretSize,
          "verified complete write lost the root secret");
  require(service.inspect() == CardRecordState::initialized_v1,
          "verified complete write did not become active");
}

void test_state_change_during_kdf_is_not_overwritten() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  store.second_read_blob.assign(32, std::byte{0x6c});
  CardRecordService service(store);

  require_state_error(
      [&] {
        static_cast<void>(service.initialize_and_authenticate(
            secure_text("service-daily-password-race")));
      },
      CardRecordState::legacy_p1_blob,
      "card state change during KDF was not reported");
  require(store.put_calls == 0,
          "card state change during KDF was overwritten");
}

void test_failed_write_preserves_original_error_when_old_record_survives() {
  SecureBuffer provisioning = marker();
  FakeCardStore store(provisioning.bytes());
  store.behavior = WriteBehavior::throw_without_write;
  CardRecordService service(store);

  require_card_error(
      [&] {
        static_cast<void>(service.initialize_and_authenticate(
            secure_text("service-daily-password-v3")));
      },
      CardError::communication_failed,
      "failed pre-commit write changed its error");
  require(service.inspect() == CardRecordState::provisioned_uninitialized,
          "failed pre-commit write changed the logical record");
}

void test_unverified_write_outcomes_fail_closed() {
  SecureBuffer provisioning = marker();
  FakeCardStore corrupted_store(provisioning.bytes());
  corrupted_store.behavior = WriteBehavior::corrupt_after_write;
  CardRecordService corrupted_service(corrupted_store);
  require_card_error(
      [&] {
        static_cast<void>(corrupted_service.initialize_and_authenticate(
            secure_text("service-daily-password-v4")));
      },
      CardError::device_failure,
      "mismatched writeback did not fail closed");

  FakeCardStore unreadable_store(provisioning.bytes());
  unreadable_store.behavior = WriteBehavior::fail_readback;
  CardRecordService unreadable_service(unreadable_store);
  require_card_error(
      [&] {
        static_cast<void>(unreadable_service.initialize_and_authenticate(
            secure_text("service-daily-password-v5")));
      },
      CardError::device_failure,
      "unreadable write outcome did not fail closed");
}

}  // namespace

int main() {
  try {
    test_public_states_are_reported_without_password_work();
    test_initialization_enters_authenticated_state();
    test_second_password_opens_the_same_record();
  test_second_password_requires_an_initialized_record();
  test_second_password_write_failure_is_verified_against_the_card();
  test_non_provisioning_states_never_initialize();
    test_verified_complete_write_error_is_accepted();
    test_state_change_during_kdf_is_not_overwritten();
    test_failed_write_preserves_original_error_when_old_record_survives();
    test_unverified_write_outcomes_fail_closed();
    std::cout << "card_record_service_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_record_service_tests failed: " << error.what()
              << '\n';
    return 1;
  }
}
