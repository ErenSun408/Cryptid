#include "card/capture_failure.h"
#include "platform/windows/startup_log.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace usb_vault::card;
using namespace usb_vault::application;
using namespace usb_vault::platform::windows;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Case { unsigned int step; NativeCodeKind kind; unsigned int native; };
// Independently audited against the 114 documented production call sites.
constexpr std::array win32_steps{1u,2u,3u,4u,5u,6u,7u,8u,10u,12u,13u,14u,16u,17u,
    18u,19u,20u,21u,23u,26u,28u,32u,34u,36u,38u,40u,42u,45u,46u,47u,48u,49u,50u,51u,53u,55u,56u,107u,108u};
static_assert(kProcessSecurityStepCount == 114, "extend the audited matrix for new steps");
class FailingSource final : public ICardEntryInspectionSource {
 public:
  Case fault;
  unsigned int cleanup_mode;
  unsigned int calls{}, cleanups{};
  FailingSource(Case value, unsigned int cleanup) : fault(value), cleanup_mode(cleanup) {}
  std::unique_ptr<IReadOnlyCardEntryInspection> inspect_inserted_card() override {
    ++calls;
    std::exception_ptr primary;
    try {
      throw_process_security(fault.step, "SENSITIVE_SENTINEL", fault.native, fault.kind);
    } catch (...) { primary = std::current_exception(); }
    if (cleanup_mode) {
      try {
        cleanup_preserving_failure(primary, [&] {
          ++cleanups;
          if (cleanup_mode == 2)
            throw_process_security(89, "SENSITIVE_CLEANUP", 0xdeadbeef, NativeCodeKind::none);
          throw DiagnosticError(make_error_code(CardError::backend_unavailable),
              DiagnosticStage::cleanup, "SENSITIVE_CLEANUP", 32, NativeCodeKind::win32);
        });
      } catch (...) { primary = std::current_exception(); }
      // A later cleanup must not overwrite the first cleanup diagnostic.
      cleanup_preserving_failure(primary, [&] {
        ++cleanups;
        throw DiagnosticError(make_error_code(CardError::backend_unavailable),
            DiagnosticStage::cleanup, "SENSITIVE_LATER_CLEANUP", 87, NativeCodeKind::win32);
      });
    }
    std::rethrow_exception(primary);
  }
};
std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "cannot read matrix log");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void run_case(const fs::path& root, Case fault, unsigned int timing, unsigned int cleanup) {
  FailingSource source(fault, cleanup);
  CardEntryContextController entry(source);
  StartupLog log;
  const SYSTEMTIME time{2026, 9, 0, 22, 13, 0, 0, 0};
  const auto path = root / L"runtime-log-20260922-130000.txt";
  require(log.open(root, time), "cannot open matrix log");
  if (timing) log.record_card_state({}, true);
  require(entry.handle_arrival() == CardEntryRoute::unavailable && !entry.has_context(),
      "injected failure accepted card context");
  const auto diagnostic = entry.startup_failure();
  const auto& actual = diagnostic.primary;
  require(actual.code == 2011 && actual.reason == 1 && actual.step == fault.step &&
      actual.native_kind == static_cast<unsigned int>(fault.kind) &&
      actual.native_code == (fault.kind == NativeCodeKind::none ? 0 : fault.native),
      "entry capture corrupted security diagnostic");
  if (timing == 0) log.record_card_state(diagnostic, true);
  if (timing == 1) {
    log.record_card_state(diagnostic);
    log.record_card_state(diagnostic); // Repeated notification snapshot.
  }
  // timing 2: no notification snapshot; page-ready must recover the diagnostic.
  log.finish_page(entry.startup_failure());
  const auto text = read(path);
  std::string expected = "；错误码：2011";
  if (fault.step) expected += "；安全步骤码：" + std::to_string(fault.step);
  expected += "；原因码：1";
  if (fault.kind != NativeCodeKind::none)
    expected += std::string(fault.kind == NativeCodeKind::win32 ? "；Win32 错误：" : "；HRESULT：") +
        std::to_string(fault.native);
  if (cleanup == 1) expected += "；附加清理错误码：2015；原因码：1；Win32 错误：32";
  if (cleanup == 2) expected += "；附加清理错误码：2011；安全步骤码：89；原因码：1";
  expected += "\r\n";
  const auto at = text.find(expected);
  require(at != std::string::npos && text.find("；错误码：2011", at + 1) == std::string::npos,
      "log lost, changed or duplicated exact diagnostic fields");
  require(at < text.find("[完成] 页面就绪") &&
      text.find("[完成] 页面就绪") < text.find("[完成] 日志结束"),
      "diagnostic was written after final markers");
  require(text.find("SENSITIVE") == std::string::npos && text.find("3735928559") == std::string::npos,
      "exception context or untyped system value leaked");
  require(source.calls == 1 && source.cleanups == (cleanup ? 2u : 0u),
      "diagnostic collection retried card inspection or cleanup");
  log.record_card_state(diagnostic);
  log.finish_page(diagnostic);
  require(read(path) == text, "completed startup accepted runtime diagnostics");
  entry.handle_removal();
  require(entry.startup_failure().primary.code == 0, "removal retained diagnostic");
  fs::remove(path);
}
}
int main() {
  const auto root = fs::current_path() / (L"security-log-matrix-" +
      std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64()));
  unsigned int count = 0;
  try {
    require(fs::create_directory(root), "matrix fixture already exists");
    std::vector<Case> cases;
    for (unsigned int step = 1; step <= kProcessSecurityStepCount; ++step) {
      const bool win32 = std::find(win32_steps.begin(), win32_steps.end(), step) != win32_steps.end();
      cases.push_back({step, step == 52 ? NativeCodeKind::hresult : win32 ? NativeCodeKind::win32 : NativeCodeKind::none,
          step == 52 ? 0x80070005u : win32 ? 5u : 0xdeadbeefu});
    }
    for (const auto fault : cases) for (unsigned int timing = 0; timing < 3; ++timing)
      for (unsigned int cleanup = 0; cleanup < 3; ++cleanup) {
        try { run_case(root, fault, timing, cleanup); }
        catch (...) {
          std::cerr << "step=" << fault.step << " kind=" << static_cast<unsigned int>(fault.kind)
              << " timing=" << timing << " cleanup=" << cleanup << '\n';
          throw;
        }
        ++count;
      }
    fs::remove(root);
    std::cout << "process_security_log_matrix_tests passed: " << cases.size()
        << " diagnostic variants, " << count << " capture-to-log scenarios\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
