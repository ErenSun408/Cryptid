#include "usb_vault/card/card_initialization_service.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_service.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using usb_vault::card::CardCapabilities;
using usb_vault::card::CardError;
using usb_vault::card::CardInitializationService;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardInitializationTarget;
using usb_vault::card::ICardStore;
using usb_vault::card::kCardRecordV1RootSecretSize;
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

class FakeCardStore final : public ICardStore {
 public:
  [[nodiscard]] SecureBuffer get_blob() override {
    ++get_calls;
    SecureBuffer result(blob.size());
    std::ranges::copy(blob, result.bytes().begin());
    return result;
  }

  void put_blob(const SecureBuffer& replacement) override {
    ++put_calls;
    blob.assign(replacement.bytes().begin(), replacement.bytes().end());
  }

  [[nodiscard]] CardCapabilities capabilities() const noexcept override {
    return {};
  }

  std::vector<std::byte> blob;
  std::size_t get_calls{};
  std::size_t put_calls{};
};

class FakeInitializationTarget final : public ICardInitializationTarget {
 public:
  void ensure_provisioning_record() override {
    ++ensure_calls;
    if (fail_bootstrap) {
      throw std::system_error(make_error_code(CardError::device_failure));
    }
    if (provisioning_verified) return;
    if (publish_malformed) {
      store.blob.assign(216, std::byte{});
    } else {
      SecureBuffer marker = provision_uninitialized_card_record_v1();
      store.blob.assign(marker.bytes().begin(), marker.bytes().end());
    }
    provisioning_verified = true;
  }

  [[nodiscard]] ICardStore& card_store() noexcept override {
    ++store_requests;
    return store;
  }

  FakeCardStore store;
  bool fail_bootstrap{};
  bool publish_malformed{};
  bool provisioning_verified{};
  std::size_t ensure_calls{};
  std::size_t store_requests{};
};

void test_bootstrap_precedes_initialized_record() {
  FakeInitializationTarget target;
  SecureBuffer root = CardInitializationService::initialize_and_authenticate(
      target, secure_text("validated-daily-password-v1"));

  require(root.size() == kCardRecordV1RootSecretSize,
          "initialization transaction returned the wrong root size");
  require(target.ensure_calls == 1 && target.store_requests == 1,
          "initialization transaction used the wrong target sequence");
  require(target.store.get_calls == 3 && target.store.put_calls == 1,
          "initialization transaction bypassed Card Record verification");
  require(usb_vault::card::inspect_card_record(
              std::span(target.store.blob)) ==
              CardRecordState::initialized_v1,
          "initialization transaction did not publish Card Record v1");
}

void test_bootstrap_failure_never_reaches_store() {
  FakeInitializationTarget target;
  target.fail_bootstrap = true;
  try {
    static_cast<void>(CardInitializationService::initialize_and_authenticate(
        target, secure_text("must-not-reach-card-record-kdf")));
    throw std::runtime_error("failed bootstrap was accepted");
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(CardError::device_failure),
            "bootstrap failure changed its card error");
  }
  require(target.store_requests == 0 && target.store.get_calls == 0 &&
              target.store.put_calls == 0,
          "bootstrap failure reached the Card Record store");
}

void test_malformed_bootstrap_result_fails_before_password_work() {
  FakeInitializationTarget target;
  target.publish_malformed = true;
  try {
    static_cast<void>(CardInitializationService::initialize_and_authenticate(
        target, secure_text("must-not-reach-malformed-record-kdf")));
    throw std::runtime_error("malformed provisioning record was accepted");
  } catch (const usb_vault::card::UnexpectedCardRecordState& error) {
    require(error.observed() == CardRecordState::malformed,
            "malformed provisioning returned the wrong state");
  }
  require(target.store.get_calls == 1 && target.store.put_calls == 0,
          "malformed provisioning reached Card Record write");
}

}  // namespace

int main() {
  try {
    test_bootstrap_precedes_initialized_record();
    test_bootstrap_failure_never_reaches_store();
    test_malformed_bootstrap_result_fails_before_password_work();
    std::cout << "card_initialization_service_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_initialization_service_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
