#include "card/capture_failure.h"
#include "usb_vault/application/card_entry_context_controller.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace usb_vault::card;
using namespace usb_vault::application;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::exception_ptr timeout_failure() {
  try {
    throw DiagnosticError(make_error_code(CardError::operation_timed_out),
        DiagnosticStage::response_timeout, "private text must not survive");
  } catch (...) { return std::current_exception(); }
}

void fail_cleanup() {
  throw DiagnosticError(make_error_code(CardError::backend_unavailable),
      DiagnosticStage::cleanup, "private path must not survive", 5,
      NativeCodeKind::win32);
}

class Inspection final : public IReadOnlyCardEntryInspection {
 public:
  bool fail{};
  CardEntryMediaEvidence media{CardEntryMediaEvidence::product_record};
  CardRecordState record{CardRecordState::initialized_v1};
  CardEntryMediaEvidence inspect_media() override {
    if (fail) std::rethrow_exception(timeout_failure());
    return media;
  }
  CardRecordState inspect_product_record_read_only() override { return record; }
};

class Source final : public ICardEntryInspectionSource {
 public:
  int calls{};
  int cleanup_calls{};
  bool fail{};
  bool empty{};
  std::unique_ptr<IReadOnlyCardEntryInspection> inspect_inserted_card() override {
    ++calls;
    if (fail) cleanup_preserving_failure(timeout_failure(), [&] {
      ++cleanup_calls;
      fail_cleanup();
    });
    if (empty) return nullptr;
    return std::make_unique<Inspection>();
  }
};

void test_entry_lifetime() {
  Source source;
  CardEntryContextController entry(source);
  require(entry.startup_failure().primary.code == 0, "new entry has error");
  source.fail = true;
  require(entry.handle_arrival() == CardEntryRoute::unavailable, "failure opened entry");
  auto result = entry.startup_failure();
  require(result.primary.code == 2019 && result.primary.reason == 18,
          "cleanup replaced original timeout");
  require(result.cleanup.code == 2015 && result.cleanup.native_kind == 1 &&
          result.cleanup.native_code == 5, "cleanup reason missing");
  require(source.calls == 1 && source.cleanup_calls == 1 && !entry.has_context(),
          "failure retried inspection or retained context");
  entry.handle_removal();
  require(entry.startup_failure().primary.code == 0 &&
          entry.startup_failure().cleanup.code == 0 && !entry.current_route(),
          "removal retained diagnostic");
  source.fail = false;
  require(entry.handle_arrival() == CardEntryRoute::login_required &&
          entry.startup_failure().primary.code == 0, "success retained old failure");
  source.empty = true;
  require(entry.handle_arrival() == CardEntryRoute::unavailable &&
          entry.startup_failure().primary.code == 4004, "null inspection not diagnosed");
}

void test_router_capture() {
  Inspection inspection;
  FailureDiagnostic failure{};
  inspection.fail = true;
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::unavailable &&
          failure.primary.code == 2019, "router swallowed failure");
  inspection.fail = false;
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::login_required &&
          failure.primary.code == 0, "router did not clear previous result");
  inspection.media = CardEntryMediaEvidence::blocked;
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::unavailable &&
          failure.primary.code == 4001, "blocked media changed route");
  inspection.media = CardEntryMediaEvidence::product_record;
  inspection.record = CardRecordState::malformed;
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::unavailable &&
          failure.primary.code == 4003, "malformed record became initializable");
  inspection.record = CardRecordState::unsupported_version;
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::unavailable &&
          failure.primary.code == 4002, "unsupported record lost diagnostic");
  inspection.media = static_cast<CardEntryMediaEvidence>(999);
  require(CardEntryRouter::route(inspection, &failure) == CardEntryRoute::unavailable &&
          failure.primary.code == 4005, "unknown media accepted or lost diagnostic");
}

void test_cleanup_paths() {
  int calls = 0;
  try {
    cleanup_preserving_failure(timeout_failure(), [&] { ++calls; });
    require(false, "primary exception lost");
  } catch (const DiagnosticError& error) {
    require(error.stage() == DiagnosticStage::response_timeout, "primary replaced");
  }
  require(calls == 1, "cleanup repeated");
  cleanup_preserving_failure({}, [&] { ++calls; });
  require(calls == 2, "success cleanup skipped");
  try {
    cleanup_preserving_failure({}, fail_cleanup);
    require(false, "standalone cleanup failure hidden");
  } catch (...) {
    const auto result = capture_failure(std::current_exception());
    require(result.primary.code == 2015 && result.cleanup.code == 0,
            "standalone cleanup failure misclassified");
  }
}

void test_numeric_sanitization() {
  static_assert(std::is_trivially_copyable_v<FailureDiagnostic>);
  static_assert(sizeof(FailureDiagnostic) == 8 * sizeof(std::uint32_t));
  try { throw std::system_error(5, std::system_category(), "private"); }
  catch (...) {
    const auto result = capture_failure(std::current_exception()).primary;
    require(result.code == 9001 && result.reason == 0 && result.native_kind == 1 &&
            result.native_code == 5, "system code mistaken for card lockout");
  }
  try { throw DiagnosticError(make_error_code(CardError::backend_unavailable),
      static_cast<DiagnosticStage>(999), "private", 12345, NativeCodeKind::none); }
  catch (...) {
    const auto result = capture_failure(std::current_exception()).primary;
    require(result.code == 9001 && result.native_kind == 0 && result.native_code == 0,
            "unknown stage or untyped detail leaked");
  }
  try { throw std::bad_alloc(); }
  catch (...) { require(capture_failure(std::current_exception()).primary.code == 9002,
                       "allocation failure lost"); }
}
}

void test_startup_fallback() {
  using namespace usb_vault::card;
  try { throw_startup_failure(1501, 1, 5); }
  catch (...) {
    const auto result = capture_startup_failure(1206).primary;
    require(result.code == 1501 && result.native_kind == 1 && result.native_code == 5 && result.reason == 0,
            "local startup error was overwritten or mistaken for card lockout");
  }
  try { throw std::runtime_error("private path and credential"); }
  catch (...) { require(capture_startup_failure(1106).primary.code == 1106, "missing fallback stage"); }
  try { throw std::bad_alloc(); }
  catch (...) { require(capture_startup_failure(1206).primary.code == 9002, "allocation failure overwritten"); }
  try { throw std::system_error(5, std::system_category(), "private"); }
  catch (...) {
    const auto result = capture_startup_failure(1104).primary;
    require(result.code == 1104 && result.native_code == 5 && result.reason == 0, "driver cause lost");
  }
}

int main() {
  try {
    test_entry_lifetime(); test_router_capture(); test_cleanup_paths();
    test_numeric_sanitization();
    test_startup_fallback();
    std::cout << "startup_failure_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
