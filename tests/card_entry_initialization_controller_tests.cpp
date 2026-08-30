#include "usb_vault/application/card_entry_context_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using usb_vault::application::CardEntryContextController;
using usb_vault::application::CardEntryMediaEvidence;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ICardEntryInspectionSource;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardCapabilities;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardInitializationTarget;
using usb_vault::card::ICardStore;
using usb_vault::card::make_error_code;
using usb_vault::card::provision_uninitialized_card_record_v1;
using usb_vault::security::ConfirmedPassword;
using usb_vault::security::IPasswordStrengthPolicy;
using usb_vault::security::PasswordStrength;
using usb_vault::security::SecureBuffer;
using usb_vault::security::confirm_password;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] SecureBuffer secure_text(const std::string_view text) {
  SecureBuffer result(text.size());
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    result.bytes().begin());
  return result;
}

class AcceptingPolicy final : public IPasswordStrengthPolicy {
 public:
  [[nodiscard]] PasswordStrength assess(
      const SecureBuffer&) const noexcept override {
    return PasswordStrength::acceptable;
  }
};

[[nodiscard]] ConfirmedPassword confirmed_password() {
  AcceptingPolicy policy;
  return confirm_password(policy, secure_text("validated-daily-password-v1"),
                          secure_text("validated-daily-password-v1"));
}

struct ActionState final {
  int inspections_destroyed{};
  int targets_destroyed{};
  int factory_calls{};
  int ensure_calls{};
  int store_requests{};
  bool controller_invalid_during_factory{};
  bool context_alive_during_factory{};
  bool context_alive_during_ensure{};
  bool target_destroyed_before_context{};
};

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  FakeInspection(ActionState& state, const CardEntryMediaEvidence media,
                 const CardRecordState record)
      : state_(state), media_(media), record_(record) {}

  ~FakeInspection() noexcept override {
    state_.target_destroyed_before_context = state_.targets_destroyed == 1;
    ++state_.inspections_destroyed;
  }

  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    return media_;
  }

  [[nodiscard]] CardRecordState
  inspect_product_record_read_only() override {
    return record_;
  }

 private:
  ActionState& state_;
  CardEntryMediaEvidence media_;
  CardRecordState record_;
};

class FakeCardStore final : public ICardStore {
 public:
  FakeCardStore() {
    SecureBuffer marker = provision_uninitialized_card_record_v1();
    blob.assign(marker.bytes().begin(), marker.bytes().end());
  }

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
  int get_calls{};
  int put_calls{};
};

class FakeInitializationTarget final : public ICardInitializationTarget {
 public:
  FakeInitializationTarget(ActionState& state, FakeCardStore& store,
                           const bool fail)
      : state_(state), store_(store), fail_(fail) {}

  ~FakeInitializationTarget() noexcept override {
    ++state_.targets_destroyed;
  }

  void ensure_provisioning_record() override {
    ++state_.ensure_calls;
    state_.context_alive_during_ensure =
        state_.inspections_destroyed == 0;
    if (fail_) {
      throw std::system_error(make_error_code(CardError::device_failure));
    }
  }

  [[nodiscard]] ICardStore& card_store() noexcept override {
    ++state_.store_requests;
    return store_;
  }

 private:
  ActionState& state_;
  FakeCardStore& store_;
  bool fail_;
};

class FakeSource final : public ICardEntryInspectionSource {
 public:
  [[nodiscard]] std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() override {
    return std::make_unique<FakeInspection>(state, media, record);
  }

  [[nodiscard]] std::unique_ptr<ICardInitializationTarget>
  make_initialization_target(
      const IReadOnlyCardEntryInspection&) override {
    ++state.factory_calls;
    state.context_alive_during_factory =
        state.inspections_destroyed == 0;
    state.controller_invalid_during_factory =
        controller != nullptr && !controller->has_context() &&
        controller->current_route() == CardEntryRoute::unavailable;
    if (throw_during_factory) {
      throw std::system_error(make_error_code(CardError::device_failure));
    }
    if (return_null_target) return nullptr;
    return std::make_unique<FakeInitializationTarget>(state, store,
                                                       fail_target);
  }

  ActionState state;
  FakeCardStore store;
  CardEntryContextController* controller{};
  CardEntryMediaEvidence media{CardEntryMediaEvidence::initializable_media};
  CardRecordState record{CardRecordState::malformed};
  bool return_null_target{};
  bool throw_during_factory{};
  bool fail_target{};
};

void require_card_error(const CardError expected,
                        auto&& operation,
                        const char* const accepted_message) {
  try {
    operation();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected),
            "initialization action returned the wrong card error");
    return;
  }
  throw std::runtime_error(accepted_message);
}

void test_success_consumes_context_before_factory_and_destroys_locals() {
  FakeSource source;
  CardEntryContextController controller(source);
  source.controller = &controller;
  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "success fixture reached the wrong route");

  SecureBuffer root =
      controller.initialize_and_authenticate(confirmed_password());

  require(root.size() == usb_vault::card::kCardRecordV1RootSecretSize,
          "initialization action returned the wrong root size");
  require(controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "successful action retained its route or context");
  require(source.state.factory_calls == 1 &&
              source.state.controller_invalid_during_factory &&
              source.state.context_alive_during_factory &&
              source.state.context_alive_during_ensure,
          "action used the context outside its one-shot lifetime");
  require(source.state.targets_destroyed == 1 &&
              source.state.inspections_destroyed == 1 &&
              source.state.target_destroyed_before_context,
          "action leaked or destroyed temporary bindings out of order");
  require(source.state.ensure_calls == 1 &&
              source.state.store_requests == 1 &&
              source.store.get_calls == 3 && source.store.put_calls == 1,
          "action bypassed the formal initialization transaction");
  require(usb_vault::card::inspect_card_record(
              std::span(source.store.blob)) ==
              CardRecordState::initialized_v1,
          "action did not publish an initialized record");

  require_card_error(
      CardError::invalid_operation,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "duplicate initialization action was accepted");
  require(source.state.factory_calls == 1,
          "duplicate action created a second target");
}

void test_wrong_routes_reject_without_consuming_context() {
  {
    FakeSource source;
    CardEntryContextController controller(source);
    require_card_error(
        CardError::invalid_operation,
        [&] {
          static_cast<void>(controller.initialize_and_authenticate(
              confirmed_password()));
        },
        "initialization without a card was accepted");
    require(!controller.current_route().has_value() &&
                !controller.has_context() &&
                source.state.factory_calls == 0,
            "no-card rejection changed controller state");
  }

  for (const auto [media, record] : {
           std::pair{CardEntryMediaEvidence::product_record,
                     CardRecordState::initialized_v1},
           std::pair{CardEntryMediaEvidence::blocked,
                     CardRecordState::malformed},
       }) {
    FakeSource source;
    source.media = media;
    source.record = record;
    CardEntryContextController controller(source);
    const CardEntryRoute original_route = controller.handle_arrival();

    require_card_error(
        CardError::invalid_operation,
        [&] {
          static_cast<void>(controller.initialize_and_authenticate(
              confirmed_password()));
        },
        "non-initialization route was accepted");
    require(controller.current_route() == original_route &&
                controller.has_context() &&
                source.state.factory_calls == 0 &&
                source.state.inspections_destroyed == 0,
            "rejected route consumed or changed its read-only context");
  }
}

void test_factory_failure_consumes_authorization_without_retry() {
  FakeSource source;
  source.throw_during_factory = true;
  CardEntryContextController controller(source);
  source.controller = &controller;
  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "factory-failure fixture reached the wrong route");

  require_card_error(
      CardError::device_failure,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "factory failure was accepted");
  require(source.state.factory_calls == 1 &&
              source.state.controller_invalid_during_factory &&
              source.state.inspections_destroyed == 1 &&
              source.state.ensure_calls == 0 &&
              controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "factory failure retained reusable authorization");

  require_card_error(
      CardError::invalid_operation,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "factory failure was retried");
  require(source.state.factory_calls == 1,
          "factory failure created another target");
}

void test_missing_target_consumes_authorization_without_retry() {
  FakeSource source;
  source.return_null_target = true;
  CardEntryContextController controller(source);
  source.controller = &controller;
  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "null-target fixture reached the wrong route");

  require_card_error(
      CardError::backend_unavailable,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "null initialization target was accepted");
  require(source.state.factory_calls == 1 &&
              source.state.controller_invalid_during_factory &&
              source.state.inspections_destroyed == 1 &&
              controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "null target left reusable authorization");

  require_card_error(
      CardError::invalid_operation,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "null-target failure was retried");
  require(source.state.factory_calls == 1,
          "null-target failure created another target");
}

void test_target_failure_consumes_authorization_without_retry() {
  FakeSource source;
  source.fail_target = true;
  CardEntryContextController controller(source);
  source.controller = &controller;
  require(controller.handle_arrival() ==
              CardEntryRoute::initialization_required,
          "failure fixture reached the wrong route");

  require_card_error(
      CardError::device_failure,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "target failure was accepted");
  require(source.state.factory_calls == 1 &&
              source.state.ensure_calls == 1 &&
              source.state.store_requests == 0 &&
              source.state.targets_destroyed == 1 &&
              source.state.inspections_destroyed == 1 &&
              source.state.target_destroyed_before_context &&
              controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "target failure retained authorization or reached the store");

  require_card_error(
      CardError::invalid_operation,
      [&] {
        static_cast<void>(controller.initialize_and_authenticate(
            confirmed_password()));
      },
      "failed target was retried");
  require(source.state.factory_calls == 1 && source.state.ensure_calls == 1,
          "failed action reused its target or context");
}

}  // namespace

int main() {
  try {
    test_success_consumes_context_before_factory_and_destroys_locals();
    test_wrong_routes_reject_without_consuming_context();
    test_factory_failure_consumes_authorization_without_retry();
    test_missing_target_consumes_authorization_without_retry();
    test_target_failure_consumes_authorization_without_retry();
    std::cout << "card_entry_initialization_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_entry_initialization_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
