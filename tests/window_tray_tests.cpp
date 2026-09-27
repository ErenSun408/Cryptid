#include "../tools/app/window_tray.h"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
std::vector<DWORD> notices;
std::vector<int> shown;
bool reject_add{}, reject_version{};
void require(bool okay, const char* message) { if (!okay) throw std::runtime_error(message); }
BOOL WINAPI notify(DWORD message, PNOTIFYICONDATAW data) {
  notices.push_back(message);
  require(std::wstring_view(data->szTip) == L"USB Vault", "tray tooltip leaked state");
  if (message == NIM_ADD) return reject_add ? FALSE : TRUE;
  if (message == NIM_SETVERSION) {
    require(data->uVersion == NOTIFYICON_VERSION_4, "wrong callback version");
    return reject_version ? FALSE : TRUE;
  }
  return TRUE;
}
BOOL WINAPI show(HWND, int state) { shown.push_back(state); return TRUE; }
}

int main() {
  // Fake shell and visibility calls: no real tray icon or user window changes.
  WindowTray tray(WM_APP + 5, notify, show);
  const HWND window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
  reject_add = true;
  require(!tray.hide(window) && !tray.hidden() && shown.empty(), "shell failure hid window");
  reject_add = false; reject_version = true; notices.clear();
  require(!tray.hide(window) && notices.back() == NIM_DELETE && shown.empty(), "version failure left unreachable icon");
  reject_version = false; notices.clear();
  require(tray.hide(window) && tray.hidden() && shown.back() == SW_HIDE, "tray did not hide");
  const auto count = notices.size();
  require(tray.hide(window) && notices.size() == count, "repeated close duplicated icon");
  tray.restore();
  require(!tray.hidden() && notices.back() == NIM_DELETE && shown.back() == SW_RESTORE, "restore did not remove icon");
  require(tray.hide(window), "second hide failed");
  notices.clear(); tray.taskbar_recreated();
  require(tray.hidden() && notices.front() == NIM_ADD, "explorer restart lost recovery icon");
  reject_add = true; tray.taskbar_recreated();
  require(!tray.hidden() && shown.back() == SW_RESTORE, "failed icon recovery stranded hidden window");
  reject_add = false; require(tray.hide(window), "final hide failed");
  tray.remove(); require(notices.back() == NIM_DELETE, "shutdown left icon");
  std::cout << "window_tray_tests: PASS\n";
}
