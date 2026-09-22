#include "card/vikey/helper_environment.h"
#include "card/vikey/vikey_helper_protocol.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace usb_vault::card::vikey::helper;
namespace {
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
using Entries = std::vector<std::wstring>;
Entries base() { return {L"LOCALAPPDATA=C:\\Fixture", L"TEMP=C:\\Fixture\\Temp", L"TMP=C:\\Fixture\\Temp"}; }
std::vector<wchar_t> block(const Entries& entries) {
  std::vector<wchar_t> out;
  for (const auto& s : entries) { out.insert(out.end(), s.begin(), s.end()); out.push_back(0); }
  out.push_back(0);
  if (entries.empty()) out.push_back(0);
  return out;
}
struct Handle {
  HANDLE value{};
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
};
// Invalid environments stop before any vendor/SDK operation. Probe has no credentials.
void check_process(const std::filesystem::path& helper, const Entries& entries, unsigned int step) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  Handle input_read, input_write, output_read, output_write, error_read, error_write;
  require(::CreatePipe(&input_read.value, &input_write.value, &sa, 0) != FALSE, "input pipe failed");
  require(::CreatePipe(&output_read.value, &output_write.value, &sa, 0) != FALSE, "output pipe failed");
  require(::SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0) != FALSE &&
      ::SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0) != FALSE, "pipe inheritance failed");
  require(::CreatePipe(&error_read.value, &error_write.value, &sa, 0) != FALSE, "error pipe failed");
  require(::SetHandleInformation(error_read.value, HANDLE_FLAG_INHERIT, 0) != FALSE, "error pipe inheritance failed");
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input_read.value; startup.hStdOutput = output_write.value;
  startup.hStdError = error_write.value;
  PROCESS_INFORMATION info{};
  auto env = block(entries);
  std::wstring command = L"\"" + helper.wstring() + L"\"";
  if (::CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(), helper.parent_path().c_str(), &startup, &info) == FALSE) {
    const auto error = ::GetLastError();
    if (step == 90 && error == ERROR_INVALID_PARAMETER) {
      std::cout << "malformed environment blocked by Windows before helper (87); classifier separately passed\n";
      return;
    }
    std::cerr << "CreateProcess error=" << error << '\n';
    throw std::runtime_error("helper launch failed");
  }
  Handle process{info.hProcess}, thread{info.hThread};
  Request request{};
  write_request(input_write.value, request);
  if (::WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0) {
    ::TerminateProcess(process.value, 1); ::WaitForSingleObject(process.value, 1000);
    throw std::runtime_error("helper environment response timed out");
  }
  DWORD available{};
  require(::PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
      available >= sizeof(Response), "helper did not produce response");
  const auto response = read_response(output_read.value, request.nonce, Operation::probe);
  require(response.diagnostic == 11 && response.status == 1 && response.security_step == step &&
      response.native_kind == 0 && response.detail == 0, "real helper misclassified environment");
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2, "helper executable required");
    const auto helper = std::filesystem::absolute(argv[1]);
    for (unsigned int step = 90; step <= 106; ++step) {
      auto entries = base();
      switch (step) {
        case 90: entries.push_back(L"MALFORMED"); break;
        case 91: entries.push_back(L"PATH=C:\\SENSITIVE"); break;
        case 92: entries[0] = L"localappdata=C:\\Fixture"; break;
        case 93: entries.push_back(entries[0]); break;
        case 94: entries.erase(entries.begin()); break;
        case 95: entries[0] = L"LOCALAPPDATA="; break;
        case 96: entries.erase(entries.begin() + 1); break;
        case 97: entries[1] = L"TEMP="; break;
        case 98: entries.erase(entries.begin() + 2); break;
        case 99: entries[2] = L"TMP="; break;
        case 100: entries[2] = L"TMP=C:\\Other"; break;
        case 101: entries[1] = L"TEMP=C:\\Other"; entries[2] = L"TMP=C:\\Other"; break;
        case 102: entries.push_back(L"WINDIR=C:\\Windows"); break;
        case 103: entries.push_back(L"SystemRoot="); entries.push_back(L"WINDIR=C:\\Windows"); break;
        case 104: entries.push_back(L"SystemRoot=C:\\Windows"); break;
        case 105: entries.push_back(L"SystemRoot=C:\\Windows"); entries.push_back(L"WINDIR="); break;
        case 106: entries.push_back(L"SystemRoot=C:\\Windows"); entries.push_back(L"WINDIR=C:\\Other"); break;
      }
      auto env = block(entries);
      unsigned int frees = 0;
      const auto result = inspect_helper_environment([&] { return env.data(); },
          [&](wchar_t*) { ++frees; return true; }, [] { return 5u; });
      require(result.step == step && result.native == 0 && frees == 1, "environment classifier mismatch");
      try { check_process(helper, entries, step); }
      catch (...) { std::cerr << "real helper environment step=" << step << '\n'; throw; }
    }
    auto entries = base();
    for (int mode = 0; mode < 2; ++mode) {
      if (mode) { entries.push_back(L"SystemRoot=C:\\Windows"); entries.push_back(L"WINDIR=C:\\Windows"); }
      auto env = block(entries);
      const auto ok = inspect_helper_environment([&] { return env.data(); }, [](wchar_t*) { return true; }, [] { return 5u; });
      require(ok.step == 0 && ok.boundary == (mode ? EnvironmentBoundary::network_enabled_development :
          EnvironmentBoundary::zero_network_app_container), "valid environment rejected");
      const auto release = inspect_helper_environment([&] { return env.data(); }, [](wchar_t*) { return false; }, [] { return 5u; });
      require(release.step == 108 && release.native == 5, "release API failure lost");
    }
    unsigned int frees = 0;
    const auto get = inspect_helper_environment([]() -> wchar_t* { return nullptr; },
        [&](wchar_t*) { ++frees; return true; }, [] { return 8u; });
    require(get.step == 107 && get.native == 8 && frees == 0, "get API failure lost or invalid block freed");
    auto invalid = block({L"SECRET=x"});
    const auto first = inspect_helper_environment([&] { return invalid.data(); }, [](wchar_t*) { return false; }, [] { return 5u; });
    require(first.step == 91 && first.native == 0, "release failure replaced primary environment rejection");
    std::cout << "helper_environment_tests passed: environment process checks, 19 environment steps, valid modes and first-error preservation\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
