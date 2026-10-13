#include "../src/platform/windows/vikey_configuration/installers.h"
#include "../src/platform/windows/vikey_configuration/setup_policy.h"
#include "../src/platform/windows/vikey_configuration/driver_deployment.h"
#include "../src/platform/windows/vikey_configuration/driver_cleanup.h"
#include "card/vikey/vikey_development_app_config.h"
#include "../src/platform/windows/installation_state.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <array>

namespace {
int diagnostic_child(std::wstring_view token, std::wstring_view mode) {
  using namespace configuration::driver;
  using configuration::setup::Result;
  Diagnostic diagnostic{Step::service_start, CodeKind::win32, 577};
  if (mode == L"hresult") diagnostic = {Step::source_verification, CodeKind::hresult, 0x800B0109, 34};
  if (mode == L"win32" || mode == L"hresult")
    return send_diagnostic(token, Result::failed, diagnostic) ? 22 : 1;
  const std::wstring name = L"\\\\.\\pipe\\USBVaultDriverDiagnostic-" + std::wstring(token);
  const auto pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 1;
  std::array<std::uint32_t, 7> packet{0x31445655, 22, 22, 1, 577, 0, 0};
  DWORD size = 24;
  if (mode == L"short") size = 8;
  else if (mode == L"long") size = 28;
  else if (mode == L"magic") packet[0] = 0;
  else if (mode == L"step") packet[2] = 1000;
  else if (mode == L"kind") packet[3] = 1000;
  else if (mode == L"result") packet[1] = 0;
  DWORD written{};
  const bool sent = WriteFile(pipe, packet.data(), size, &written, nullptr) && written == size;
  CloseHandle(pipe);
  return sent ? 22 : 1;
}
void diagnostic_tests() {
  using namespace configuration::driver;
  using namespace usb_vault::card;
  using configuration::setup::Result;
  if (valid_diagnostic_token(L"") || valid_diagnostic_token(std::wstring(32, L'G')) ||
      valid_diagnostic_token(L"../../unexpected") ||
      send_diagnostic(L"bad", Result::failed, {})) throw std::runtime_error("unsafe diagnostic token accepted");
  Diagnostic diagnostic{Step::target_verification};
  const DiagnosticError certificate(make_error_code(CardError::backend_unavailable), DiagnosticStage::installer_signature,
      "private-path-must-never-escape", 0x80092013, NativeCodeKind::hresult);
  capture_exception(diagnostic, certificate);
  if (format_diagnostic(diagnostic) != "UVD1|20|2|0x80092013|34")
    throw std::runtime_error("HRESULT or diagnostic privacy lost");
  diagnostic = {Step::target_create};
  capture_exception(diagnostic, std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "private-path"));
  if (format_diagnostic(diagnostic) != "UVD1|15|1|0x00000005|0")
    throw std::runtime_error("Win32 diagnostic lost");
  std::array<wchar_t, 32768> executable{};
  if (!GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())))
    throw std::runtime_error("diagnostic child path unavailable");
  for (const auto mode : {L"win32", L"hresult", L"short", L"long", L"magic", L"step", L"kind", L"result", L"peer"}) {
    DiagnosticReceiver receiver;
    if (!valid_diagnostic_token(receiver.token())) throw std::runtime_error("bad generated diagnostic token");
    const std::wstring child_mode = std::wstring_view(mode) == L"peer" ? L"win32" : mode;
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --diagnostic-child " + receiver.token() + L" " + child_mode;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
          nullptr, nullptr, &startup, &process)) throw std::runtime_error("diagnostic child failed to start");
    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess);
      throw std::runtime_error("diagnostic child timed out");
    }
    DWORD code{};
    GetExitCodeProcess(process.hProcess, &code);
    Diagnostic received{Step::wait_elevated, CodeKind::process_exit, 22};
    const auto pid = std::wstring_view(mode) == L"peer" ? GetCurrentProcessId() : process.dwProcessId;
    const bool accepted = receiver.receive(pid, code, received);
    CloseHandle(process.hProcess);
    if (code != 22) throw std::runtime_error("diagnostic child send failed");
    const bool wanted = std::wstring_view(mode) == L"win32" || std::wstring_view(mode) == L"hresult";
    if (accepted != wanted) throw std::runtime_error("invalid diagnostic report disposition");
    if (!accepted && (received.step != Step::wait_elevated || received.code != 22))
      throw std::runtime_error("rejected report modified fallback diagnostic");
    if (accepted && std::wstring_view(mode) == L"hresult" &&
        format_diagnostic(received) != "UVD1|12|2|0x800B0109|34") throw std::runtime_error("child HRESULT truncated");
    if (accepted && std::wstring_view(mode) == L"win32" &&
        format_diagnostic(received) != "UVD1|22|1|0x00000241|0") throw std::runtime_error("child Win32 code lost");
  }
  DiagnosticReceiver empty;
  diagnostic = {Step::wait_elevated};
  if (empty.receive(GetCurrentProcessId(), 22, diagnostic)) throw std::runtime_error("absent report accepted");
}
void helper_diagnostic_tests(const wchar_t* executable) {
  using namespace configuration::driver;
  HANDLE token{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("cannot inspect test token");
  TOKEN_ELEVATION elevation{};
  DWORD count{};
  const bool inspected = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &count) != FALSE;
  CloseHandle(token);
  if (!inspected) throw std::runtime_error("cannot inspect test elevation");
  if (elevation.TokenIsElevated) {
    std::cout << "SKIP real helper pipe report: elevated test token must not install a system driver\n";
    return;
  }
  DiagnosticReceiver receiver;
  std::wstring command = L"\"" + std::wstring(executable) + L"\" --install-driver-elevated --diagnostic-pipe " + receiver.token();
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
      nullptr, nullptr, &startup, &process)) throw std::runtime_error("real diagnostic helper did not start");
  CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess);
    throw std::runtime_error("real diagnostic helper timed out");
  }
  DWORD code{};
  GetExitCodeProcess(process.hProcess, &code);
  Diagnostic received{};
  const bool accepted = receiver.receive(process.dwProcessId, code, received);
  CloseHandle(process.hProcess);
  if (code != 12 || !accepted || format_diagnostic(received) != "UVD1|11|1|0x000002E4|0")
    throw std::runtime_error("real helper lost token rejection or original error across pipe");
  std::cout << "PASS real helper pipe preserved elevation rejection and Win32 740; no driver access\n";
}
struct RemovalFixture {
  configuration::driver_cleanup::State state;
  int fail{};
  std::vector<int> calls;
  configuration::driver_cleanup::State inspect() { calls.push_back(0); return state; }
  bool step(int number) { calls.push_back(number); return number != fail; }
  bool verify() { return step(1); }
  bool idle() { return step(2); }
  bool stop() { return step(3); }
  bool stopped() { return step(4); }
  bool prepare_delete() { return step(5); }
  bool delete_service() { return step(6); }
  bool delete_file() { return step(7); }
  bool absent() { return step(8); }
};
void removal_policy_tests() {
  using namespace configuration::driver_cleanup;
  using configuration::setup::Result;
  for (auto state : {State::absent, State::blocked, State::running, State::stopped, State::orphan}) {
    std::vector<int> sequence{0};
    if (state != State::absent && state != State::blocked) {
      sequence.push_back(1);
      if (state == State::running) { sequence.push_back(2); sequence.push_back(3); }
      sequence.push_back(4); sequence.push_back(5);
      if (state != State::orphan) sequence.push_back(6);
      sequence.push_back(7); sequence.push_back(8);
    }
    for (int fail = 0; fail <= 8; ++fail) {
      RemovalFixture ops{state, fail, {}};
      const auto result = remove(ops);
      auto expected = sequence;
      auto match = std::find(expected.begin() + 1, expected.end(), fail);
      if (match != expected.end()) expected.erase(match + 1, expected.end());
      // Check against the original sequence, avoiding invalidated iterators.
      const bool reached = std::find(sequence.begin() + 1, sequence.end(), fail) != sequence.end();
      const auto wanted = state == State::blocked ? Result::unavailable : reached ?
          (fail >= 6 ? Result::failed : Result::unavailable) : Result::ready;
      if (ops.calls != expected || result != wanted) throw std::runtime_error("unsafe driver removal sequencing");
    }
  }
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--diagnostic-child")
    return diagnostic_child(argv[2], argv[3]);
  if (argc == 3 && std::wstring_view(argv[1]) == L"--helper-diagnostics") {
    try { helper_diagnostic_tests(argv[2]); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  const auto directory = std::filesystem::temp_directory_path() /
      (L"usb-vault-installer-test-" + std::to_wstring(::GetCurrentProcessId()));
  bool owned = false;
  try {
    diagnostic_tests();
    removal_policy_tests();
    using configuration::setup::Result;
    using configuration::setup::ensure_dependency;
    using configuration::driver::Presence;
    using configuration::driver::classify;
    if (classify(Presence::absent, false) != Result::missing ||
        classify(Presence::absent, true) != Result::unavailable ||
        classify(Presence::trusted_running, false) != Result::ready ||
        classify(Presence::trusted_stopped, false) != Result::repairable ||
        classify(Presence::existing_unavailable, false) != Result::unavailable ||
        classify(Presence::unknown, false) != Result::unavailable ||
        classify(Presence::incompatible, false) != Result::incompatible)
      throw std::runtime_error("unsafe driver disposition");
    for (auto initial : {Result::ready, Result::incompatible, Result::unavailable}) {
      int installs = 0;
      const auto result = ensure_dependency([&] { return initial; }, [&]() -> Result {
        ++installs; return Result::failed;
      });
      if (installs != 0) throw std::runtime_error("setup overwrote an existing dependency");
      if (result != initial) throw std::runtime_error("probe result changed");
    }
    for (auto installed : {Result::ready, Result::restart, Result::cancelled, Result::failed}) {
      int probes = 0;
      const auto result = ensure_dependency([&] { return ++probes == 1 ? Result::missing : Result::ready; },
                                           [&] { return installed; });
      if (result != installed || probes != (installed == Result::ready ? 2 : 1))
        throw std::runtime_error("installer result or recheck was lost");
    }
    Result missing = Result::missing;
    Result reported_ready = Result::ready;
    if (ensure_dependency([&] { return missing; }, [&] { return reported_ready; }) != Result::failed)
      throw std::runtime_error("installer success bypassed availability check");
    using configuration::InstallState;
    using configuration::classify_installer_exit;
    if (classify_installer_exit(0).state != InstallState::success ||
        classify_installer_exit(3010).state != InstallState::reboot_required ||
        classify_installer_exit(1641).state != InstallState::reboot_required ||
        classify_installer_exit(1602).state != InstallState::cancelled ||
        classify_installer_exit(1223).state != InstallState::cancelled ||
        classify_installer_exit(1603).state != InstallState::failed ||
        classify_installer_exit(999).exit_code != 999)
      throw std::runtime_error("installer exit result misclassified");
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("fixture already exists");
    owned = true;
    using usb_vault::platform::windows::installation_ready_for_start;
    if (!installation_ready_for_start(directory)) throw std::runtime_error("clean install blocked");
    const auto transaction = directory / L".usb-vault-update";
    std::filesystem::create_directory(transaction);
    if (installation_ready_for_start(directory)) throw std::runtime_error("pending update allowed startup");
    std::filesystem::remove(transaction);
    const auto lock = directory / L".usb-vault-setup.lock";
    { std::ofstream file(lock); file << "locked"; }
    if (installation_ready_for_start(directory)) throw std::runtime_error("installer lock allowed startup");
    std::filesystem::remove(lock);
    for (const bool exists : {false, true}) {
      const auto fixture = directory / L"untrusted.exe";
      if (exists) { std::ofstream file(fixture, std::ios::binary); file << "untrusted installer"; }
      bool rejected = false;
      try { static_cast<void>(configuration::verify_veracrypt_driver(fixture)); }
      catch (const usb_vault::card::DiagnosticError& error) {
        rejected = true;
        if (!exists && (error.stage() != usb_vault::card::DiagnosticStage::file_open ||
            error.kind() != usb_vault::card::NativeCodeKind::win32 || error.native() != ERROR_FILE_NOT_FOUND))
          throw std::runtime_error("missing driver native error lost");
      }
      if (!rejected) throw std::runtime_error("untrusted or missing driver accepted");
      rejected = false;
      try { static_cast<void>(configuration::verify_webview_installer(fixture)); }
      catch (...) { rejected = true; }
      if (!rejected) throw std::runtime_error("untrusted WebView2 installer accepted");
    }
    if (argc >= 2) {
      const auto verified = configuration::verify_veracrypt_driver(argv[1]);
      std::cout << "Reviewed installer hash and Authenticode verified (not executed)\n";
    }
    if (argc >= 2) {
      const auto copy = directory / L"signed-removal.sys";
      std::filesystem::copy_file(argv[1], copy);
      { auto token = configuration::verify_veracrypt_driver(copy, true);
        if (!token.remove_verified_file()) throw std::runtime_error("signed driver handle deletion failed"); }
      if (std::filesystem::exists(copy)) throw std::runtime_error("verified driver copy retained");
    }
    const auto plain = directory / L"verified-removal.txt";
    { std::ofstream file(plain); file << "abc"; }
    const auto digest = usb_vault::card::vikey::development::parse_deployment_digest(
        L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    auto wrong_digest = digest; wrong_digest.front() ^= std::byte{1};
    bool removal_rejected = false;
    try { static_cast<void>(usb_vault::card::verify_local_artifact(plain, wrong_digest, 3, true)); }
    catch (...) { removal_rejected = true; }
    if (!removal_rejected || !std::filesystem::exists(plain)) throw std::runtime_error("mismatched removal modified file");
    if (!SetFileAttributesW(plain.c_str(), FILE_ATTRIBUTE_READONLY)) throw std::runtime_error("readonly removal fixture failed");
    removal_rejected = false;
    try { static_cast<void>(usb_vault::card::verify_local_artifact(plain, digest, 3, true)); }
    catch (...) { removal_rejected = true; }
    if (!SetFileAttributesW(plain.c_str(), FILE_ATTRIBUTE_NORMAL)) throw std::runtime_error("readonly restore failed");
    if (!removal_rejected) throw std::runtime_error("readonly file passed removal preflight");
    { auto token = usb_vault::card::verify_local_artifact(plain, digest, 3);
      if (token.remove_verified_file()) throw std::runtime_error("read verification granted deletion"); }
    const auto outside = directory / L"self-stored.txt";
    if (!CreateHardLinkW(outside.c_str(), plain.c_str(), nullptr)) throw std::runtime_error("removal hardlink fixture failed");
    { auto token = usb_vault::card::verify_local_artifact(plain, digest, 3, true);
      HANDLE write = CreateFileW(plain.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr, OPEN_EXISTING, 0, nullptr);
      if (write != INVALID_HANDLE_VALUE) { CloseHandle(write); throw std::runtime_error("removal token allowed concurrent writes"); }
      if (!token.remove_verified_file()) throw std::runtime_error("verified handle deletion failed"); }
    if (std::filesystem::exists(plain)) throw std::runtime_error("verified file retained");
    if (std::filesystem::file_size(outside) != 3) throw std::runtime_error("removal altered hardlink data");
    std::filesystem::remove(outside);
    if (argc == 3) {
      const auto verified = configuration::verify_webview_installer(argv[2]);
      std::cout << "WebView2 installer hash and Authenticode verified (not executed)\n";
    }
    std::filesystem::remove(directory / L"untrusted.exe");
    std::filesystem::remove(directory);
    std::cout << "configuration_installer_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::error_code ignored;
    if (owned) {
      std::filesystem::remove(directory / L"untrusted.exe", ignored);
      std::filesystem::remove(directory, ignored);
    }
    std::cerr << error.what() << '\n';
    return 1;
  }
}
