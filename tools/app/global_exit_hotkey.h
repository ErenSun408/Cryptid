#pragma once

#include <Windows.h>
#include "usb_vault/ui/exit_hotkey.h"
#include <future>
#include <atomic>
#include <thread>

// Dedicated hook thread: the UI may be busy with card or shell calls. Never
// perform IO or cleanup in the callback, and always pass keys to other apps.
class GlobalExitHotkey final {
 public:
  ~GlobalExitHotkey() { stop(); }
  bool configure(HWND target, UINT message, usb_vault::application::ExitHotkeySettings settings) noexcept {
    stop();
    ++generation_;
    pending_.store(false);
    if (!settings.enabled) return true;
    if (!usb_vault::application::valid_exit_hotkey(settings)) return false;
    try {
      std::promise<bool> ready;
      auto result = ready.get_future();
      thread_ = std::thread([this, target, message, settings, ready = std::move(ready)]() mutable {
        MSG queue{};
        ::PeekMessageW(&queue, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        thread_id_ = ::GetCurrentThreadId();
        usb_vault::ui::ExitHotkeyCounter counter(settings);
        for (unsigned int key = 0; key < 256; ++key)
          // LL notifications identify left/right modifiers, never their
          // aggregate VK_SHIFT/CONTROL/MENU aliases. Do not latch aliases.
          if ((key < VK_SHIFT || key > VK_MENU) &&
              (::GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0) counter.held(key);
        Context context{&counter, target, message, generation_, &pending_, &capture_};
        context_ = &context;
        const HHOOK hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, ::GetModuleHandleW(nullptr), 0);
        ready.set_value(hook != nullptr);
        if (hook) {
          while (::GetMessageW(&queue, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&queue);
            ::DispatchMessageW(&queue);
          }
          ::UnhookWindowsHookEx(hook);
        }
        context_ = nullptr;
      });
      if (result.get()) return true;
      stop();
    } catch (...) { stop(); }
    return false;
  }
  void set_capture(bool active) noexcept { capture_.store(active); }
  void stop() noexcept {
    if (thread_.joinable()) {
      ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
      thread_.join();
    }
  }
  WPARAM generation() const noexcept { return generation_; }
  bool consume(WPARAM generation) noexcept {
    return generation == generation_ && pending_.exchange(false);
  }
 private:
  struct Context {
    usb_vault::ui::ExitHotkeyCounter* counter;
    HWND target;
    UINT message;
    WPARAM generation;
    std::atomic<bool>* pending;
    std::atomic<bool>* capture;
  };
  static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) noexcept {
    if (code == HC_ACTION && context_ &&
        (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN || wparam == WM_KEYUP || wparam == WM_SYSKEYUP)) {
      const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
      if (context_->counter->input(key.vkCode, wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN,
                                  (key.flags & LLKHF_INJECTED) != 0, ::GetTickCount64(),
                                  context_->capture->load() && ::GetForegroundWindow() == context_->target)) {
        if (!context_->pending->exchange(true) &&
            !::PostMessageW(context_->target, context_->message, context_->generation, 0))
          context_->pending->store(false);
      }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  inline static thread_local Context* context_{};
  std::thread thread_;
  DWORD thread_id_{};
  WPARAM generation_{};
  std::atomic<bool> pending_{};
  std::atomic<bool> capture_{};
};
