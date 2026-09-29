#include "../src/platform/windows/vikey_configuration/installers.h"
#include "../src/platform/windows/vikey_configuration/setup_policy.h"
#include "../src/platform/windows/vikey_configuration/driver_deployment.h"
#include "../src/platform/windows/installation_state.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
  const auto directory = std::filesystem::temp_directory_path() /
      (L"usb-vault-installer-test-" + std::to_wstring(::GetCurrentProcessId()));
  bool owned = false;
  try {
    using configuration::setup::Result;
    using configuration::setup::ensure_dependency;
    using configuration::driver::Presence;
    using configuration::driver::classify;
    if (classify(Presence::absent, false) != Result::missing ||
        classify(Presence::absent, true) != Result::unavailable ||
        classify(Presence::trusted_running, false) != Result::ready ||
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
      try { static_cast<void>(configuration::verify_veracrypt_installer(fixture)); }
      catch (...) { rejected = true; }
      if (!rejected) throw std::runtime_error("untrusted or missing installer accepted");
      rejected = false;
      try { static_cast<void>(configuration::verify_webview_installer(fixture)); }
      catch (...) { rejected = true; }
      if (!rejected) throw std::runtime_error("untrusted WebView2 installer accepted");
    }
    if (argc >= 2) {
      const auto verified = configuration::verify_veracrypt_installer(argv[1]);
      std::cout << "Reviewed installer hash and Authenticode verified (not executed)\n";
    }
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
