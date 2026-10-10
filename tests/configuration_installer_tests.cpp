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

namespace {
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
  const auto directory = std::filesystem::temp_directory_path() /
      (L"usb-vault-installer-test-" + std::to_wstring(::GetCurrentProcessId()));
  bool owned = false;
  try {
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
      catch (...) { rejected = true; }
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
