#include "usb_vault/application/card_entry_context_controller.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_service.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/card/card_record_v2.h"

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
using usb_vault::application::ICardLoginSuccessConsumer;
using usb_vault::application::IReadOnlyCardEntryInspection;
using usb_vault::card::CardCredentialRejected;
using usb_vault::card::CardError;
using usb_vault::card::CardRecordState;
using usb_vault::card::ICardLoginTarget;
using usb_vault::card::InitializedCardRecordV1;
using usb_vault::card::UnexpectedCardRecordState;
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

struct State final {
  CardEntryContextController* controller{};
  int put_calls{};
  std::vector<std::byte> record;
  int factory_calls{};
  int read_calls{};
  int targets_destroyed{};
  int inspections_destroyed{};
  bool throw_in_factory{};
  bool return_null_target{};
  bool throw_on_read{};
  std::vector<std::byte> second_read_record;
  bool v2_entry{};
  usb_vault::card::CardProductRecordState target_format{usb_vault::card::CardProductRecordState::initialized_v1};
  int completion_calls{};
  int completion_failure{};
};

class FakeInspection final : public IReadOnlyCardEntryInspection {
 public:
  FakeInspection(State& state, const CardEntryMediaEvidence media,
                 const CardRecordState logical)
      : state_(state), media_(media), logical_(logical) {}

  ~FakeInspection() noexcept override { ++state_.inspections_destroyed; }

  [[nodiscard]] CardEntryMediaEvidence inspect_media() override {
    return media_;
  }

  [[nodiscard]] CardRecordState inspect_product_record_read_only() override {
    return logical_;
  }
  usb_vault::card::CardProductRecordState inspect_supported_product_record_read_only() override {
    return state_.v2_entry ? usb_vault::card::CardProductRecordState::initialized_v2 :
        IReadOnlyCardEntryInspection::inspect_supported_product_record_read_only();
  }

 private:
  State& state_;
  CardEntryMediaEvidence media_;
  CardRecordState logical_;
};

class FakeLoginTarget final : public ICardLoginTarget {
 public:
  explicit FakeLoginTarget(State& state) noexcept : state_(state) {}
  ~FakeLoginTarget() noexcept override { ++state_.targets_destroyed; }
  usb_vault::card::CardProductRecordState record_format() const noexcept override { return state_.target_format; }
  void complete_authenticated_read(std::span<const std::byte> expected, const SecureBuffer& root) override {
    ++state_.completion_calls;
    require(root.size() == 32 && copy_bytes(expected) == state_.record, "v2 gate received wrong root shape or record");
    if (state_.completion_failure == 1) throw CardCredentialRejected();
    if (state_.completion_failure == 2) ICardLoginTarget::complete_authenticated_read(expected, root);
    if (state_.completion_failure == 3) throw std::system_error(usb_vault::card::make_error_code(CardError::communication_failed));
  }

  [[nodiscard]] SecureBuffer read_record() override {
    ++local_reads_;
    ++state_.read_calls;
    if (state_.throw_on_read) {
      throw std::system_error(
          usb_vault::card::make_error_code(CardError::communication_failed));
    }
    const std::vector<std::byte>& source =
        local_reads_ == 2 && !state_.second_read_record.empty()
            ? state_.second_read_record
            : state_.record;
    SecureBuffer result(source.size());
    std::ranges::copy(source, result.bytes().begin());
    return result;
  }

 private:
  State& state_;
  int local_reads_{};
};

class FakeRecordStore final : public usb_vault::card::ICardStore {
 public:
  explicit FakeRecordStore(State& state) noexcept : state_(state) {}

  [[nodiscard]] SecureBuffer get_blob() override {
    SecureBuffer result(state_.record.size());
    std::ranges::copy(state_.record, result.bytes().begin());
    return result;
  }

  void put_blob(const SecureBuffer& blob) override {
    ++state_.put_calls;
    state_.record.assign(blob.bytes().begin(), blob.bytes().end());
  }

  [[nodiscard]] usb_vault::card::CardCapabilities capabilities()
      const noexcept override {
    return {};
  }

 private:
  State& state_;
};

class FakeUpdateTarget final : public usb_vault::card::ICardUpdateTarget {
 public:
  explicit FakeUpdateTarget(State& state) noexcept : store_(state) {}

  [[nodiscard]] usb_vault::card::ICardStore& card_store() noexcept override {
    return store_;
  }

 private:
  FakeRecordStore store_;
};

class FakeSource final : public ICardEntryInspectionSource {
 public:
  [[nodiscard]] std::unique_ptr<IReadOnlyCardEntryInspection>
  inspect_inserted_card() override {
    return std::make_unique<FakeInspection>(state, media, logical);
  }

  [[nodiscard]] std::unique_ptr<ICardLoginTarget> make_login_target(
      const IReadOnlyCardEntryInspection&) override {
    ++state.factory_calls;
    require(state.controller != nullptr &&
                state.controller->current_route() ==
                    CardEntryRoute::login_required &&
                state.controller->has_context(),
            "login context was invalidated before target construction");
    if (state.throw_in_factory) {
      throw std::system_error(
          usb_vault::card::make_error_code(CardError::device_failure));
    }
    if (state.return_null_target) return nullptr;
    return std::make_unique<FakeLoginTarget>(state);
  }

  [[nodiscard]] std::unique_ptr<usb_vault::card::ICardUpdateTarget>
  make_update_target(const IReadOnlyCardEntryInspection&) override {
    return std::make_unique<FakeUpdateTarget>(state);
  }

  State state;
  CardEntryMediaEvidence media{CardEntryMediaEvidence::product_record};
  CardRecordState logical{CardRecordState::initialized_v1};
};

class RecordingConsumer final : public ICardLoginSuccessConsumer {
 public:
  explicit RecordingConsumer(CardEntryContextController& controller)
      : controller_(controller) {}

  void consume_authenticated_login(
      SecureBuffer root_secret,
      SecureBuffer login_password_utf8) override {
    ++calls;
    invalid_before_call =
        controller_.current_route() == CardEntryRoute::unavailable &&
        !controller_.has_context();
    root = copy_bytes(root_secret.bytes());
    password = copy_bytes(login_password_utf8.bytes());
    session.establish(std::move(root_secret));
  }

  CardEntryContextController& controller_;
  int calls{};
  bool invalid_before_call{};
  std::vector<std::byte> root;
  std::vector<std::byte> password;
  usb_vault::application::CardAuthenticationSession session;
};

template <typename Action>
void require_card_error(const CardError expected, Action&& action,
                        const char* const message) {
  try {
    action();
  } catch (const std::system_error& error) {
    require(error.code() == usb_vault::card::make_error_code(expected),
            message);
    return;
  }
  throw std::runtime_error(message);
}

void test_credential_rejection_preserves_context_for_retry(
    const InitializedCardRecordV1& initialized) {
  FakeSource source;
  source.state.record = copy_bytes(initialized.record.bytes());
  CardEntryContextController controller(source);
  source.state.controller = &controller;
  RecordingConsumer consumer(controller);
  require(controller.handle_arrival() == CardEntryRoute::login_required,
          "login fixture reached the wrong route");

  try {
    controller.authenticate_login(consumer, secure_text("wrong-password"));
  } catch (const CardCredentialRejected&) {
    require(controller.current_route() == CardEntryRoute::login_required &&
                controller.has_context() && consumer.calls == 0 &&
                source.state.factory_calls == 1 &&
                source.state.read_calls == 1 &&
                source.state.targets_destroyed == 1 &&
                source.state.inspections_destroyed == 0,
            "credential rejection consumed its retryable context");

    controller.authenticate_login(
        consumer, secure_text("correct-login-password"));
    require(consumer.calls == 1 && consumer.invalid_before_call &&
                consumer.root == copy_bytes(initialized.root_secret.bytes()) &&
                consumer.password ==
                    copy_bytes(secure_text("correct-login-password").bytes()) &&
                controller.current_route() == CardEntryRoute::unavailable &&
                !controller.has_context() &&
                source.state.factory_calls == 2 &&
                source.state.read_calls == 3 &&
                source.state.targets_destroyed == 2 &&
                source.state.inspections_destroyed == 1,
            "successful retry did not perform the one-shot handoff");
    return;
  }
  throw std::runtime_error("wrong login password was accepted");
}

void test_success_recheck_rejects_record_change(
    const InitializedCardRecordV1& initialized) {
  FakeSource source;
  source.state.record = copy_bytes(initialized.record.bytes());
  SecureBuffer marker =
      usb_vault::card::provision_uninitialized_card_record_v1();
  source.state.second_read_record = copy_bytes(marker.bytes());
  CardEntryContextController controller(source);
  source.state.controller = &controller;
  RecordingConsumer consumer(controller);
  require(controller.handle_arrival() == CardEntryRoute::login_required,
          "record-change fixture reached the wrong route");

  require_card_error(
      CardError::device_failure,
      [&] {
        controller.authenticate_login(
            consumer, secure_text("correct-login-password"));
      },
      "record change during login was accepted");
  require(controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context() && consumer.calls == 0 &&
              source.state.factory_calls == 1 &&
              source.state.read_calls == 2 &&
              source.state.targets_destroyed == 1 &&
              source.state.inspections_destroyed == 1,
          "record change retained context or delivered secrets");
}

void test_noncredential_failures_invalidate_context() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    FakeSource source;
    SecureBuffer marker =
        usb_vault::card::provision_uninitialized_card_record_v1();
    source.state.record = copy_bytes(marker.bytes());
    source.state.throw_in_factory = scenario == 0;
    source.state.return_null_target = scenario == 1;
    source.state.throw_on_read = scenario == 2;
    if (scenario == 3) {
      source.state.record.assign(
          usb_vault::card::kCardRecordV1Size, std::byte{});
    }

    CardEntryContextController controller(source);
    source.state.controller = &controller;
    RecordingConsumer consumer(controller);
    require(controller.handle_arrival() == CardEntryRoute::login_required,
            "failure fixture reached the wrong route");

    bool rejected = false;
    try {
      controller.authenticate_login(consumer, secure_text("unused-password"));
    } catch (const UnexpectedCardRecordState&) {
      require(scenario == 3, "unexpected state escaped the wrong scenario");
      rejected = true;
    } catch (const std::system_error& error) {
      if (scenario == 0) {
        require(error.code() == usb_vault::card::make_error_code(
                                    CardError::device_failure),
                "factory failure changed its error");
      } else if (scenario == 1) {
        require(error.code() == usb_vault::card::make_error_code(
                                    CardError::backend_unavailable),
                "missing target changed its error");
      } else if (scenario == 2) {
        require(error.code() == usb_vault::card::make_error_code(
                                    CardError::communication_failed),
                "read failure changed its error");
      } else {
        throw;
      }
      rejected = true;
    }

    require(rejected &&
                controller.current_route() == CardEntryRoute::unavailable &&
                !controller.has_context() && consumer.calls == 0 &&
                source.state.inspections_destroyed == 1,
            "noncredential failure retained a reusable context");
  }
}

void test_login_update_uses_a_fresh_inspection(
    const InitializedCardRecordV1& initialized) {
  FakeSource source;
  source.state.record = copy_bytes(initialized.record.bytes());
  CardEntryContextController controller(source);
  source.state.controller = &controller;
  RecordingConsumer consumer(controller);
  require(controller.handle_arrival() == CardEntryRoute::login_required,
          "login fixture reached the wrong route");

  controller.authenticate_login(consumer,
                                secure_text("correct-login-password"));
  require(consumer.calls == 1, "login did not hand off");

  require(controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "login retained its one-shot insertion");

  // Updating the record remains reachable, but through a new adapter
  // inspection rather than the context consumed by login.
  controller.set_second_card_password(secure_text("correct-login-password"),
                                      secure_text("second-login-password"));
  require(source.state.put_calls == 1,
          "a logged-in session could not rewrite the card record");
  require(source.state.inspections_destroyed == 2 &&
              controller.current_route() == CardEntryRoute::unavailable &&
              !controller.has_context(),
          "the record rewrite did not retire its fresh inspection");
}

void test_wrong_routes_reject_without_target_creation() {
  for (const auto [media, logical] : {
           std::pair{CardEntryMediaEvidence::initializable_media,
                     CardRecordState::malformed},
           std::pair{CardEntryMediaEvidence::blocked,
                     CardRecordState::malformed},
       }) {
    FakeSource source;
    source.media = media;
    source.logical = logical;
    CardEntryContextController controller(source);
    source.state.controller = &controller;
    RecordingConsumer consumer(controller);
    const CardEntryRoute original_route = controller.handle_arrival();

    require_card_error(
        CardError::invalid_operation,
        [&] {
          controller.authenticate_login(consumer,
                                        secure_text("unused-password"));
        },
        "non-login route was accepted");
    require(controller.current_route() == original_route &&
                controller.has_context() &&
                source.state.factory_calls == 0 && consumer.calls == 0,
            "route precondition consumed context or created a target");
  }
}

void test_v2_login_handoff_and_failure_boundaries(const InitializedCardRecordV1& legacy) {
  using namespace usb_vault::card;
  auto initial = initialize_card_record_and_root_v2(secure_text("correct-login-password"));
  FakeSource source;
  source.state.v2_entry = true;
  source.state.target_format = CardProductRecordState::initialized_v2;
  source.state.record = copy_bytes(initial.record.bytes());
  CardEntryContextController controller(source);
  source.state.controller = &controller;
  RecordingConsumer consumer(controller);
  require(controller.handle_arrival() == CardEntryRoute::login_required, "v2 entry did not select login");
  bool wrong_rejected = false;
  try { controller.authenticate_login(consumer, secure_text("wrong-password")); }
  catch (const CardCredentialRejected&) { wrong_rejected = true; }
  require(wrong_rejected && controller.has_context() && source.state.completion_calls == 0 &&
      consumer.calls == 0 && !consumer.session.authenticated(), "wrong v2 password reached recovery or session");
  controller.authenticate_login(consumer, secure_text("correct-login-password"));
  require(consumer.calls == 1 && consumer.invalid_before_call && consumer.session.authenticated() &&
      consumer.root == copy_bytes(initial.root_secret.bytes()) && source.state.completion_calls == 1 &&
      source.state.read_calls == 3 && source.state.targets_destroyed == 2 && source.state.inspections_destroyed == 1,
      "v2 login did not verify, retire, and hand off once");
  require(consumer.password == copy_bytes(secure_text("correct-login-password").bytes()), "v2 changed exact login input");

  for (int scenario = 1; scenario <= 4; ++scenario) {
    FakeSource rejected;
    rejected.state.v2_entry = true;
    rejected.state.target_format = CardProductRecordState::initialized_v2;
    rejected.state.record = copy_bytes(initial.record.bytes());
    if (scenario <= 3) rejected.state.completion_failure = scenario;
    else { rejected.state.second_read_record = rejected.state.record; rejected.state.second_read_record.back() ^= std::byte{1}; }
    CardEntryContextController failed(rejected);
    rejected.state.controller = &failed;
    RecordingConsumer unused(failed);
    static_cast<void>(failed.handle_arrival());
    bool refused = false;
    try { failed.authenticate_login(unused, secure_text("correct-login-password")); }
    catch (const std::exception&) { refused = true; }
    require(refused && !failed.has_context() && failed.current_route() == CardEntryRoute::unavailable &&
        unused.calls == 0 && !unused.session.authenticated() && rejected.state.completion_calls == (scenario == 4 ? 0 : 1) &&
        rejected.state.targets_destroyed == 1 && rejected.state.inspections_destroyed == 1,
        "v2 anchor, missing gate, recovery or recheck failure remained retryable or delivered root");
  }
  for (const auto format : {CardProductRecordState::initialized_v1, static_cast<CardProductRecordState>(999),
                           CardProductRecordState::initialized_v2}) {
    FakeSource incompatible;
    incompatible.state.v2_entry = format != CardProductRecordState::initialized_v2;
    incompatible.state.target_format = format;
    incompatible.state.record = copy_bytes(format == CardProductRecordState::initialized_v1 ?
        legacy.record.bytes() : initial.record.bytes());
    CardEntryContextController failed(incompatible);
    incompatible.state.controller = &failed;
    RecordingConsumer unused(failed);
    static_cast<void>(failed.handle_arrival());
    bool refused = false;
    try { failed.authenticate_login(unused, secure_text("correct-login-password")); }
    catch (const std::exception&) { refused = true; }
    require(refused && !failed.has_context() && unused.calls == 0 && incompatible.state.completion_calls == 0,
        "v2 bytes implicitly promoted an old or unknown target");
    require(incompatible.state.read_calls == 0, "unknown or mismatched format performed I/O");
  }
}

}  // namespace

int main() {
  try {
    InitializedCardRecordV1 initialized =
        usb_vault::card::initialize_card_record_and_root_v1(
            secure_text("correct-login-password"));
    test_credential_rejection_preserves_context_for_retry(initialized);
    test_success_recheck_rejects_record_change(initialized);
    test_noncredential_failures_invalidate_context();
    test_login_update_uses_a_fresh_inspection(initialized);
    test_wrong_routes_reject_without_target_creation();
    test_v2_login_handoff_and_failure_boundaries(initialized);
    std::cout << "card_entry_login_controller_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_entry_login_controller_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
