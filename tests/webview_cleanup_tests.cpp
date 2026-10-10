#include "platform/windows/vikey_configuration/webview_cleanup.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary); file << "fixture bytes";
  check(file.good(), "fixture write failed");
}
}
int wmain(int argc, wchar_t** argv) {
  using namespace configuration::webview_cleanup;
  using configuration::setup::Result;
  const auto fixture = std::filesystem::temp_directory_path() /
      (L"usb-vault-webview-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  bool owned = false;
  try {
    for (auto version : {L"1.2.3.4", L"142.0.3595.94", L"1.0.0.0"}) check(valid_version(version), "valid version rejected");
    for (auto version : {L"", L"0.0.0.0", L"1.2.3", L"1.2.3.4.5", L"..", L"1.2.3.4 --uninstall", L"1.2.3/4", L"1..3.4", L"1.2.3.4\"", L"1.2.3.4:ads"})
      check(!valid_version(version), "unsafe version accepted");
    for (auto initial : {Result::missing, Result::unavailable, Result::incompatible}) {
      int runs = 0, checks = 0;
      const auto result = remove_runtime([&] { return initial; }, [&] { ++runs; return Result::ready; }, [&] { ++checks; return true; });
      check(result == (initial == Result::missing ? Result::ready : initial) && runs == 0 && checks == 0,
            "unsafe initial runtime state executed uninstall");
    }
    for (auto action : {Result::ready, Result::failed, Result::cancelled, Result::unavailable, Result::restart}) {
      for (bool absent : {false, true}) {
        int checks = 0;
        Result ready = Result::ready;
        const auto result = remove_runtime([&] { return ready; }, [&] { return action; }, [&] { ++checks; return absent; });
        check(result == (action == Result::ready ? (absent ? Result::ready : Result::failed) : action), "uninstall outcome lost");
        check(checks == (action == Result::ready ? 1 : 0), "absence check was skipped or premature");
      }
    }
    check(std::filesystem::create_directory(fixture), "fixture collision"); owned = true;
    const auto application = fixture / L"app";
    std::filesystem::create_directory(application);
    const auto profile = application / L"USB Vault.exe.WebView2";
    const auto data = profile / L"EBWebView";
    write(application / L"container.hc");
    write(application / L"self-stored.txt");
    check(clean_profile(application) == Result::ready, "missing cache blocked cleanup");
    write(data / L"Default" / L"Cache" / L"entry");
    write(data / L"Local State");
    write(profile / L"self-stored.txt");
    check(clean_profile(application) == Result::ready, "profile cleanup failed");
    check(!std::filesystem::exists(data) && std::filesystem::exists(profile / L"self-stored.txt"), "profile sibling was deleted");
    check(std::filesystem::exists(application / L"container.hc") && std::filesystem::exists(application / L"self-stored.txt"), "unrelated file deleted");
    std::filesystem::remove(profile / L"self-stored.txt");
    check(clean_profile(application) == Result::ready && !std::filesystem::exists(profile), "empty profile retained");
    write(data / L"Default" / L"a"); write(data / L"Default" / L"b");
    const auto locked_path = data / L"Default" / L"b";
    HANDLE lock = CreateFileW(locked_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "fixture lock failed");
    const auto locked_result = clean_profile(application);
    CloseHandle(lock);
    check(locked_result != Result::ready && std::filesystem::exists(data / L"Default" / L"a"), "locked cache partially deleted during preflight");
    check(clean_profile(application) == Result::ready, "cache retry failed");
    write(data / L"a"); write(data / L"readonly");
    check(SetFileAttributesW((data / L"readonly").c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "readonly fixture failed");
    check(clean_profile(application) != Result::ready && std::filesystem::exists(data / L"a"), "readonly cache was partially deleted");
    check(SetFileAttributesW((data / L"readonly").c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "readonly restore failed");
    check(clean_profile(application) == Result::ready, "readonly cache retry failed");
    write(profile); // A file cannot masquerade as the profile directory.
    check(clean_profile(application) != Result::ready && std::filesystem::is_regular_file(profile), "file profile accepted");
    std::filesystem::remove(profile);
    write(data / L"plain");
    const auto outside = fixture / L"outside";
    write(outside);
    check(CreateHardLinkW((data / L"link").c_str(), outside.c_str(), nullptr) != FALSE, "hardlink fixture failed");
    check(clean_profile(application) == Result::ready && std::filesystem::file_size(outside) == 13, "hardlink cleanup modified outside data");
    check(clean_profile(application / L"..") != Result::ready && clean_profile(L"relative") != Result::ready, "aliased root accepted");
    write(fixture / L"unsigned.exe");
    check(!microsoft_signed_file(fixture / L"unsigned.exe") && !microsoft_signed_file(fixture / L"absent.exe"), "untrusted executable accepted");
    const auto path = setup_path(application, L"142.0.3595.94");
    check(path == application / L"Microsoft\\EdgeWebView\\Application\\142.0.3595.94\\Installer\\setup.exe", "fixed setup path changed");
    bool rejected = false;
    try { static_cast<void>(setup_path(application, L"..\\attacker")); } catch (...) { rejected = true; }
    check(rejected, "setup path injection accepted");
    if (argc == 2) check(microsoft_signed_file(argv[1]), "official Microsoft signature rejected");
    std::filesystem::remove_all(fixture);
    std::cout << "PASS WebView cleanup policy, paths, signatures, cache scope, locks, hardlinks and retry (no system uninstall)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    // Leave failed evidence in the owned fixture; never delete an unknown path.
    if (owned) std::wcerr << fixture.native() << L'\n';
    return 1;
  }
}
