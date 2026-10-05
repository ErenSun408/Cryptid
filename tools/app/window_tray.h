#pragma once
#include <Windows.h>
#include <shellapi.h>

// UI-thread only. Register a reachable icon before hiding the window. The
// tooltip deliberately contains no card, container, mount or session facts.
class WindowTray final {
 public:
  using Notify = BOOL (WINAPI*)(DWORD, PNOTIFYICONDATAW);
  using Show = BOOL (WINAPI*)(HWND, int);
  explicit WindowTray(UINT callback, Notify notify = ::Shell_NotifyIconW,
                      Show show = ::ShowWindow) noexcept
      : callback_(callback), notify_(notify), show_(show) {}
  bool hide(HWND window) noexcept {
    if (hidden_) return true;
    window_ = window;
    if (!add()) return false;
    maximized_ = ::IsZoomed(window) != FALSE;
    hidden_ = true;
    show_(window_, SW_HIDE);
    return true;
  }
  void restore() noexcept {
    if (!hidden_) return;
    hidden_ = false;
    remove();
    show_(window_, maximized_ ? SW_SHOWMAXIMIZED : SW_RESTORE);
    ::SetForegroundWindow(window_);
  }
  void taskbar_recreated() noexcept {
    registered_ = false;
    if (hidden_ && !add()) restore();
  }
  void remove() noexcept {
    if (registered_) {
      auto data = descriptor();
      notify_(NIM_DELETE, &data);
      registered_ = false;
    }
  }
  bool hidden() const noexcept { return hidden_; }
 private:
  NOTIFYICONDATAW descriptor() const noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data); data.hWnd = window_; data.uID = 1;
    data.uCallbackMessage = callback_;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
    if (!data.hIcon) data.hIcon = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(data.szTip, L"USB Vault");
    return data;
  }
  bool add() noexcept {
    auto data = descriptor();
    if (!notify_(NIM_ADD, &data)) return false;
    registered_ = true;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (notify_(NIM_SETVERSION, &data)) return true;
    remove();
    return false;
  }
  UINT callback_;
  Notify notify_;
  Show show_;
  HWND window_{};
  bool registered_{}, hidden_{}, maximized_{};
};
