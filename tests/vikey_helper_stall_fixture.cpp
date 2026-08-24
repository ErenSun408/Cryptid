#include <Windows.h>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                    _In_ int) {
  static_cast<void>(::SleepEx(INFINITE, FALSE));
  return 1;
}
