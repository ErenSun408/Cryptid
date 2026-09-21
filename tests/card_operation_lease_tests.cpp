#include "card/vikey/card_operation_lease.h"
#include <thread>
#include <string>
#include <iostream>
#include <stdexcept>
using usb_vault::card::vikey::CardOperationLease;
int wmain(int argc, wchar_t** argv) {
  if (argc == 2) {
    try { CardOperationLease lease(argv[1]); return 2; }
    catch (const std::system_error&) { return 0; }
  }
  try {
    const auto name = L"Local\\USBVault.LeaseTest." + std::to_wstring(::GetCurrentProcessId());
    {
      CardOperationLease outer(name.c_str());
      CardOperationLease nested(name.c_str());
      bool rejected = false;
      std::thread other([&] { try { CardOperationLease conflict(name.c_str()); }
        catch (const std::system_error&) { rejected = true; } });
      other.join();
      if (!rejected) throw std::runtime_error("cross-thread exclusion missing");
      wchar_t path[32768]{};
      if (!::GetModuleFileNameW(nullptr, path, 32768)) throw std::runtime_error("module path");
      std::wstring command = L"\"" + std::wstring(path) + L"\" " + name;
      STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
      if (!::CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
          nullptr, nullptr, &startup, &process)) throw std::runtime_error("child launch");
      const auto waited = ::WaitForSingleObject(process.hProcess, 10000);
      DWORD code = 1; ::GetExitCodeProcess(process.hProcess, &code);
      ::CloseHandle(process.hThread); ::CloseHandle(process.hProcess);
      if (waited != WAIT_OBJECT_0 || code != 0) throw std::runtime_error("cross-process exclusion missing");
    }
    CardOperationLease after(name.c_str());
    std::cout << "card_operation_lease_tests passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what(); return 1; }
}
