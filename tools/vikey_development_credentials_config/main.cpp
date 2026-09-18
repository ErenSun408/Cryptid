#include "platform/windows/vikey_configuration/window.h"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  static_cast<void>(::SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
  static_cast<void>(configuration::run_window(instance, show_command, false));
  return 0;
}
