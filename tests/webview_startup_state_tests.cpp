#include "ui/webview_startup_state.h"
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    usb_vault::ui::WebviewStartupState state;
    require(usb_vault::ui::page_startup_error(L"1408") == 1408 &&
            usb_vault::ui::page_startup_error(L"1402") == 1402 &&
            usb_vault::ui::page_startup_error(L"\"1408\"") == 0 &&
            usb_vault::ui::page_startup_error(L"1408secret") == 0 &&
            usb_vault::ui::page_startup_error(L"9999") == 0,
            "untrusted page result treated as a diagnostic");
    require(!state.fail(0) && !state.ready(0), "inactive callback accepted");
    state.begin(1, 100);
    require(!state.expired(1, 30099) && state.expired(1, 30100), "timeout boundary wrong");
    require(state.fail(1) && !state.fail(1), "failure notice repeated");
    require(!state.ready(1) && !state.current(1), "failed page resumed");
    state.begin(2, 40000);
    require(!state.ready(1) && !state.fail(1) && !state.expired(1, 90000),
            "retired page altered replacement state");
    require(state.ready(2) && !state.expired(2, 90000), "ready page timed out");
    require(state.fail(2), "renderer failure after ready was ignored");
    state.begin(3, 100000);
    state.stop();
    require(!state.ready(3) && !state.fail(3) && !state.expired(3, 200000),
            "closed window accepted callbacks");
    std::cout << "webview_startup_state_tests passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
