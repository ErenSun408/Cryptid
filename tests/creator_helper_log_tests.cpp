#include "usb_vault/diagnostics/debug_log.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Starts the real helper without the pipe arguments it needs. It refuses
// before any privileged step, which is exactly the path its own log must
// explain: when it started, where it stopped and with which exit code.
void test_helper_logs_its_early_exit(const std::filesystem::path& helper) {
  std::wstring command = L"\"" + helper.native() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // The helper's manifest asks for elevation. Running it as the invoker is
  // enough here: it refuses at argument parsing, before any privileged step.
  require(::SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker") != FALSE,
          "cannot request invoker-level start");
  const BOOL started = ::CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  const DWORD start_error = ::GetLastError();
  ::SetEnvironmentVariableW(L"__COMPAT_LAYER", nullptr);
  require(started != FALSE,
          "cannot start the volume-creation helper: Win32 " + std::to_string(start_error));
  ::CloseHandle(process.hThread);
  require(::WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0,
          "the helper did not exit");
  DWORD exit_code = 0;
  ::GetExitCodeProcess(process.hProcess, &exit_code);
  ::CloseHandle(process.hProcess);
  require(exit_code == 2, "the helper accepted a command line without its pipe");

  const std::wstring prefix = L"creator-log-" + std::to_wstring(process.dwProcessId) + L"-";
  std::vector<std::filesystem::path> logs;
  for (const auto& entry : std::filesystem::directory_iterator(helper.parent_path())) {
    if (entry.path().filename().native().starts_with(prefix)) logs.push_back(entry.path());
  }
#if USB_VAULT_DEBUG_LOG
  require(logs.size() == 1, "the helper did not write exactly one log of its own");
  std::string text;
  {
    std::ifstream in(logs.front(), std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::string pid = std::to_string(process.dwProcessId);
  for (const std::string& expected :
       {std::string("\xEF\xBB\xBFUSB Vault 运行日志\r\n"),
        "[创建助手进程] 启动；进程号 " + pid + "；命令行 ",
        std::string("[创建助手进程] 提前退出；管道参数；退出码 2；Win32 "),
        std::string("[创建助手进程] 退出；退出码 2")}) {
    require(text.find(expected) != std::string::npos, "the helper log lacks: " + expected);
  }
  for (const auto& log : logs) std::filesystem::remove(log);
#else
  require(logs.empty(), "a release helper wrote a debug log");
#endif
}

}  // namespace

int wmain(const int argc, wchar_t** const argv) {
  try {
    require(argc == 2, "usage: creator_helper_log_tests <helper>");
    test_helper_logs_its_early_exit(argv[1]);
    std::cout << "creator_helper_log_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "creator_helper_log_tests failed: " << error.what() << '\n';
    return 1;
  }
}
