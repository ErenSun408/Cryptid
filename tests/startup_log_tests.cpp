#include "platform/windows/startup_log.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace usb_vault::platform::windows;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(file.good(), "cannot read log");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
int main() {
  const auto root = fs::current_path() / (L"startup-log-tests-" + std::to_wstring(::GetCurrentProcessId()) +
      L"-" + std::to_wstring(::GetTickCount64()));
  try {
    require(fs::create_directory(root), "fixture already exists");
    const SYSTEMTIME time{2026, 9, 0, 20, 17, 12, 34, 0};
    const auto first = root / L"startup-log-20260920-171234.txt";
    const auto second = root / L"startup-log-20260920-171234-01.txt";
    StartupLog log;
    require(log.open(root, time), "cannot create startup log");
    StartupLog collision;
    require(collision.open(root, time), "same-second launch failed instead of choosing suffix");
    collision.finish();
    require(fs::exists(first) && fs::exists(second), "timestamp naming failed");
    log.record(StartupStep::session, StartupOutcome::failed,
        {.primary = {.code = 2019, .reason = 18, .native_kind = 0, .native_code = 0xdeadbeef},
         .cleanup = {.code = 2015, .reason = 1, .native_kind = 1, .native_code = 5}});
    const auto flushed = read(first);
    require(flushed.find("code=2019 reason=18 native_kind=0 native_code=0") != std::string::npos &&
            flushed.find("cleanup_code=2015 cleanup_reason=1 cleanup_kind=1 cleanup_native=5") != std::string::npos,
            "failure diagnostics not flushed or cleanup overwritten");
    require(flushed.find("3735928559") == std::string::npos, "untyped IPC detail leaked");
    log.record(StartupStep::session, StartupOutcome::completed,
        {.primary = {.code = 0, .native_kind = 1, .native_code = 0xdeadbeef}});
    require(read(first).find("3735928559") == std::string::npos, "success payload leaked into diagnostics");
    log.finish();
    const auto finished = read(first);
    log.record(StartupStep::failure, StartupOutcome::failed, {.primary = {.code = 3005}});
    log.finish();
    require(read(first) == finished, "runtime activity appended after startup ended");
    StartupLog bounded;
    require(bounded.open(root, time), "cannot create bounded log");
    for (int index = 0; index < 1000; ++index) bounded.record(StartupStep::webview, StartupOutcome::begin);
    bounded.finish();
    const auto third = root / L"startup-log-20260920-171234-02.txt";
    const auto limited = read(third);
    require(limited.size() < 65536 && limited.find("seq=127 ") != std::string::npos &&
            limited.find("seq=128 ") == std::string::npos, "log exceeded fixed record budget");
    StartupLog unavailable;
    require(!unavailable.open(root / L"missing", time) && unavailable.error() != 0,
            "unwritable destination falsely reported success");
    unavailable.record(StartupStep::entry, StartupOutcome::begin);
    unavailable.finish();
    require(!unavailable.open(first, time), "regular file accepted as log directory");
    require(read(first) == finished, "failed logging overwrote existing file");
    // Remove only exact files owned by this fixture; no recursive deletion.
    fs::remove(first); fs::remove(second); fs::remove(third); fs::remove(root);
    std::cout << "startup_log_tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
