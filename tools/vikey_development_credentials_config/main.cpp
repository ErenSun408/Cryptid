#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_session.h"

#include <Windows.h>
#include <objbase.h>

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
#include "diagnostics.h"
#include "installers.h"
#include <atomic>
#include <mutex>
#include <thread>
#endif

namespace {

using usb_vault::card::vikey::VikeyCredentialSet;
using usb_vault::card::vikey::kVikeyApiKeyBytes;
using usb_vault::card::vikey::kVikeyUserPinBytes;
using usb_vault::card::vikey::kVikeyUserSnBytes;
using usb_vault::card::vikey::development::CredentialField;
using usb_vault::card::vikey::development::store_credentials;
using usb_vault::card::vikey::development::valid_credential_value;
using usb_vault::security::SecureBuffer;

constexpr wchar_t kWindowClass[] =
    L"UsbVaultVikeyDevelopmentCredentialsConfig";
constexpr wchar_t kWindowTitle[] =
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
    L"USB Vault - 配置与检测（开发版）";
#else
    L"USB Vault - ViKey 开发凭据配置";
#endif

constexpr int kUserSnEditId = 1001;
constexpr int kApiKeyEditId = 1002;
constexpr int kUserPinEditId = 1003;
constexpr int kSaveButtonId = 1004;
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
constexpr int kCheckButtonId = 1005;
constexpr int kLaunchButtonId = 1006;
constexpr int kInstallButtonId = 1007;
constexpr int kWebviewButtonId = 1008;
#endif

struct UiState final {
  HWND user_sn{};
  HWND api_key{};
  HWND user_pin{};
  HWND save{};
  HWND status{};
  HFONT font{};
  bool saving{};
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
  HWND check{};
  HWND launch{};
  HWND results{};
  HWND install{};
  HWND webview{};
  bool installing{};
  std::thread worker;
  std::atomic<bool> checking{false};
  std::mutex mutex;
  std::wstring report;
  bool ready{};
  ~UiState() { if (worker.joinable()) worker.join(); }
#endif
};

[[noreturn]] void fail_windows(const char* const context) {
  throw std::system_error(static_cast<int>(::GetLastError()),
                          std::system_category(), context);
}

[[nodiscard]] HWND create_control(
    const DWORD extended_style, const wchar_t* const class_name,
    const wchar_t* const text, const DWORD style, const int x, const int y,
    const int width, const int height, const HWND parent, const int id,
    const HINSTANCE instance) {
  const HWND control = ::CreateWindowExW(
      extended_style, class_name, text, style, x, y, width, height, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  if (control == nullptr) fail_windows("create credential control");
  return control;
}

void set_control_font(const HWND control, const HFONT font) noexcept {
  static_cast<void>(::SendMessageW(
      control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE));
}

[[nodiscard]] SecureBuffer read_ascii_field(
    const HWND edit, const CredentialField field,
    const std::size_t expected_characters) {
  const int text_length = ::GetWindowTextLengthW(edit);
  if (text_length < 0 ||
      static_cast<std::size_t>(text_length) != expected_characters) {
    throw std::invalid_argument("credential text has the wrong length");
  }

  SecureBuffer wide_storage(
      (expected_characters + 1) * sizeof(wchar_t));
  auto* const wide_text =
      reinterpret_cast<wchar_t*>(wide_storage.data());
  const int copied = ::GetWindowTextW(
      edit, wide_text, static_cast<int>(expected_characters + 1));
  if (copied < 0 || static_cast<std::size_t>(copied) != expected_characters) {
    throw std::runtime_error("credential text changed while being read");
  }

  SecureBuffer ascii(expected_characters);
  for (std::size_t index = 0; index < expected_characters; ++index) {
    const wchar_t value = wide_text[index];
    if (value == L'\0' || value > 0x7f) {
      throw std::invalid_argument("credential text is not ASCII");
    }
    ascii.bytes()[index] =
        static_cast<std::byte>(static_cast<unsigned int>(value));
  }
  if (!valid_credential_value(field, ascii.bytes())) {
    throw std::invalid_argument("credential text has invalid characters");
  }
  return ascii;
}

[[nodiscard]] VikeyCredentialSet read_all_fields(const UiState& state) {
  return {
      .user_sn = read_ascii_field(
          state.user_sn, CredentialField::user_sn, kVikeyUserSnBytes),
      .api_key = read_ascii_field(
          state.api_key, CredentialField::api_key, kVikeyApiKeyBytes),
      .user_pin = read_ascii_field(
          state.user_pin, CredentialField::user_pin, kVikeyUserPinBytes),
  };
}

[[nodiscard]] bool fields_are_valid(const UiState& state) noexcept {
  try {
    static_cast<void>(read_all_fields(state));
    return true;
  } catch (...) {
    return false;
  }
}

void update_save_button(UiState& state) noexcept {
  const bool enabled = !state.saving && fields_are_valid(state);
  static_cast<void>(::EnableWindow(state.save, enabled ? TRUE : FALSE));
}

void clear_fields(const UiState& state) noexcept {
  static_cast<void>(::SetWindowTextW(state.user_sn, L""));
  static_cast<void>(::SetWindowTextW(state.api_key, L""));
  static_cast<void>(::SetWindowTextW(state.user_pin, L""));
}

void save(UiState& state) noexcept {
  if (state.saving) return;
  state.saving = true;
  update_save_button(state);
  try {
    store_credentials(read_all_fields(state));
    clear_fields(state);
    static_cast<void>(::SetWindowTextW(
        state.status,
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
        L"已保存并复读验证。请插卡并运行检测。"));
#else
        L"已保存并复读验证。现在可以启动 USB Vault 开发程序。"));
#endif
  } catch (const std::system_error& error) {
    const std::wstring message =
        L"保存失败（Windows 错误 " +
        std::to_wstring(error.code().value()) +
        L"）。部分凭据可能已更新，请重新保存全部三项。";
    static_cast<void>(::SetWindowTextW(state.status, message.c_str()));
  } catch (...) {
    static_cast<void>(::SetWindowTextW(
        state.status,
        L"保存失败。请检查三项格式，然后重新保存全部三项。"));
  }
  state.saving = false;
  update_save_button(state);
}

#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
void check_environment(UiState& state) noexcept {
  if (state.checking.load()) return;
  if (::GetWindowTextLengthW(state.user_sn) || ::GetWindowTextLengthW(state.api_key) ||
      ::GetWindowTextLengthW(state.user_pin)) {
    ::SetWindowTextW(state.status, L"请先保存填写的凭据；检测使用当前用户已保存的配置。");
    return;
  }
  if (state.worker.joinable()) state.worker.join();
  state.ready = false;
  ::EnableWindow(state.launch, FALSE);
  ::EnableWindow(state.check, FALSE);
  ::EnableWindow(state.save, FALSE);
  ::EnableWindow(state.install, FALSE);
  ::EnableWindow(state.webview, FALSE);
  for (const HWND field : {state.user_sn, state.api_key, state.user_pin}) ::EnableWindow(field, FALSE);
  ::SetWindowTextW(state.status, L"检测运行中，请等待；开发版安全卡访问可能联网。检测不改写卡。");
  state.checking.store(true);
  try {
    state.worker = std::thread([&state] {
      const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      try {
        if (FAILED(com)) throw std::runtime_error("COM unavailable");
        std::array<configuration::Result, configuration::count> results{};
        state.ready = configuration::diagnose([&](unsigned int index, const configuration::Result& result) {
          results[index] = result;
          constexpr const wchar_t* names[] = {L"系统架构", L"运行组件", L"WebView2", L"VeraCrypt 驱动", L"已保存凭据", L"安全卡连接", L"安全卡访问"};
          std::wstring text;
          for (unsigned int item = 0; item < configuration::count; ++item) {
            const auto& entry = results[item];
            const wchar_t* mark = entry.state == configuration::State::passed ? L"[通过] " :
                entry.state == configuration::State::failed ? L"[未通过] " :
                entry.state == configuration::State::running ? L"[检测中] " :
                entry.state == configuration::State::skipped ? L"[跳过] " : L"[等待] ";
            text += std::wstring(mark) + names[item] + L"：" + entry.detail + L"\r\n\r\n";
          }
          const std::lock_guard lock(state.mutex);
          state.report = std::move(text);
        });
      } catch (...) {
        const std::lock_guard lock(state.mutex);
        state.report = L"检测无法完成，请检查环境后重试。";
      }
      if (SUCCEEDED(com)) ::CoUninitialize();
      state.checking.store(false);
    });
  } catch (...) {
    state.checking.store(false);
  }
}

void install_driver(UiState& state) noexcept {
  if (state.checking.load()) return;
  if (state.worker.joinable()) state.worker.join();
  state.ready = false;
  state.installing = true;
  for (const HWND control : {state.check, state.launch, state.save, state.install,
                            state.webview, state.user_sn, state.api_key, state.user_pin})
    ::EnableWindow(control, FALSE);
  ::SetWindowTextW(state.status, L"正在校验安装器，随后将出现管理员授权和安装窗口。");
  ::SetWindowTextW(state.results, L"请在安装器中选择 Install。若要求重启，请先重启 Windows；完成后重新运行检测。");
  state.checking.store(true);
  try {
    state.worker = std::thread([&state] {
      const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      std::wstring result;
      try {
        if (FAILED(com)) throw std::runtime_error("COM unavailable");
        result = configuration::install_veracrypt(configuration::executable_directory())
            ? L"安装器已退出。请按安装器提示完成操作，必要时重启 Windows，然后重新运行检测。"
            : L"已取消管理员授权。未启动安装器，可稍后重试。";
      } catch (...) {
        result = L"无法校验或启动安装器。请确认 prerequisites 中有原版安装器，系统时间及证书验证网络正常，或重新获取完整测试包。";
      }
      if (SUCCEEDED(com)) ::CoUninitialize();
      { const std::lock_guard lock(state.mutex); state.report = std::move(result); }
      state.checking.store(false);
    });
  } catch (...) {
    { const std::lock_guard lock(state.mutex); state.report = L"无法启动安装任务，请稍后重试。"; }
    state.checking.store(false);
  }
}
#endif

void initialize_controls(const HWND window, UiState& state,
                         const HINSTANCE instance) {
  state.font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));

  const HWND heading = create_control(
      0, L"STATIC", L"配置当前 Windows 用户的 ViKey 开发凭据",
      WS_CHILD | WS_VISIBLE, 24, 20, 570, 24, window, 0, instance);
  const HWND warning = create_control(
      0, L"STATIC",
      L"仅限开发卡和非生产材料。三个值会同时写入 Windows 凭据管理器。",
      WS_CHILD | WS_VISIBLE, 24, 50, 570, 34, window, 0, instance);
  const HWND user_sn_label = create_control(
      0, L"STATIC", L"UserSN（32 个十六进制字符）",
      WS_CHILD | WS_VISIBLE, 24, 92, 570, 20, window, 0, instance);
  state.user_sn = create_control(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
      24, 114, 570, 27, window, kUserSnEditId, instance);

  const HWND api_key_label = create_control(
      0, L"STATIC", L"API Key（128 个十六进制字符）",
      WS_CHILD | WS_VISIBLE, 24, 153, 570, 20, window, 0, instance);
  state.api_key = create_control(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
      24, 175, 570, 27, window, kApiKeyEditId, instance);

  const HWND user_pin_label = create_control(
      0, L"STATIC", L"用户 PIN（8 个字母或数字）",
      WS_CHILD | WS_VISIBLE, 24, 214, 570, 20, window, 0, instance);
  state.user_pin = create_control(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
      24, 236, 570, 27, window, kUserPinEditId, instance);

  state.save = create_control(
      0, L"BUTTON", L"保存开发凭据",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      444, 282, 150, 32, window, kSaveButtonId, instance);
  state.status = create_control(
      0, L"STATIC", L"请填写全部三项；格式正确后才可保存。",
      WS_CHILD | WS_VISIBLE, 24, 286, 405, 44, window, 0, instance);
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
  state.check = create_control(0, L"BUTTON", L"运行检测", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      24, 340, 150, 32, window, kCheckButtonId, instance);
  state.launch = create_control(0, L"BUTTON", L"启动 USB Vault", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      424, 340, 170, 32, window, kLaunchButtonId, instance);
  state.results = create_control(WS_EX_CLIENTEDGE, L"EDIT",
      L"已配置过的用户可直接运行检测。需要更新配置时，请先填写并保存全部三项。\r\n缺失依赖可使用安装入口处理，完成后重新检测。",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
      24, 430, 570, 208, window, 0, instance);
  state.install = create_control(0, L"BUTTON", L"安装 VeraCrypt（管理员）", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      24, 386, 275, 32, window, kInstallButtonId, instance);
  state.webview = create_control(0, L"BUTTON", L"获取 WebView2（微软官网）", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
      309, 386, 285, 32, window, kWebviewButtonId, instance);
  for (const HWND control : {state.check, state.launch, state.results, state.install, state.webview}) set_control_font(control, state.font);
  ::EnableWindow(state.launch, FALSE);
  if (::SetTimer(window, 1, 150, nullptr) == 0) fail_windows("create diagnostic timer");
#endif

  for (const HWND control :
       {heading, warning, user_sn_label, state.user_sn, api_key_label,
        state.api_key, user_pin_label, state.user_pin, state.save,
        state.status}) {
    set_control_font(control, state.font);
  }

  static_cast<void>(::SendMessageW(
      state.user_sn, EM_SETLIMITTEXT,
      static_cast<WPARAM>(kVikeyUserSnBytes), 0));
  static_cast<void>(::SendMessageW(
      state.api_key, EM_SETLIMITTEXT,
      static_cast<WPARAM>(kVikeyApiKeyBytes), 0));
  static_cast<void>(::SendMessageW(
      state.user_pin, EM_SETLIMITTEXT,
      static_cast<WPARAM>(kVikeyUserPinBytes), 0));
  update_save_button(state);
  static_cast<void>(::SetFocus(state.user_sn));
}

LRESULT CALLBACK window_procedure(const HWND window, const UINT message,
                                  const WPARAM w_param,
                                  const LPARAM l_param) noexcept {
  if (message == WM_NCCREATE) {
    const auto* const create =
        reinterpret_cast<const CREATESTRUCTW*>(l_param);
    static_cast<void>(::SetWindowLongPtrW(
        window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(create->lpCreateParams)));
  }
  auto* const state = reinterpret_cast<UiState*>(
      ::GetWindowLongPtrW(window, GWLP_USERDATA));

  switch (message) {
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
    case WM_TIMER:
      if (state && w_param == 1) {
        {
          const std::lock_guard lock(state->mutex);
          if (!state->report.empty()) {
            ::SetWindowTextW(state->results, state->report.c_str());
            state->report.clear();
          }
        }
        if (!state->checking.load() && !::IsWindowEnabled(state->check)) {
          if (state->worker.joinable()) state->worker.join();
          for (const HWND field : {state->user_sn, state->api_key, state->user_pin, state->check, state->install, state->webview}) ::EnableWindow(field, TRUE);
          update_save_button(*state);
          ::EnableWindow(state->launch, state->ready ? TRUE : FALSE);
          ::SetWindowTextW(state->status, state->installing ? L"安装任务已结束，请查看结果并重新检测。" :
              state->ready ? L"检测通过，可以启动主程序。" : L"请处理未通过的项目，然后重新检测。");
          state->installing = false;
        }
        return 0;
      }
      break;
#endif
    case WM_CREATE:
      try {
        const auto* const create =
            reinterpret_cast<const CREATESTRUCTW*>(l_param);
        initialize_controls(window, *state, create->hInstance);
        return 0;
      } catch (...) {
        return -1;
      }

    case WM_COMMAND:
      if (state == nullptr) break;
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
      if (state->checking.load()) return 0;
      if (HIWORD(w_param) == BN_CLICKED && LOWORD(w_param) == kInstallButtonId) {
        install_driver(*state); return 0;
      }
      if (HIWORD(w_param) == BN_CLICKED && LOWORD(w_param) == kWebviewButtonId) {
        state->ready = false;
        ::EnableWindow(state->launch, FALSE);
        try {
          configuration::open_webview_download();
          ::SetWindowTextW(state->status, L"请在微软官网下载安装 WebView2 Runtime，完成后重新检测。");
        } catch (...) { ::SetWindowTextW(state->status, L"无法打开浏览器，请按测试须知中的微软官网链接下载。"); }
        return 0;
      }
      if (HIWORD(w_param) == BN_CLICKED && LOWORD(w_param) == kCheckButtonId) {
        check_environment(*state); return 0;
      }
      if (HIWORD(w_param) == BN_CLICKED && LOWORD(w_param) == kLaunchButtonId) {
        if (!state->ready) return 0;
        try { configuration::launch_application(); }
        catch (...) { ::SetWindowTextW(state->status, L"启动失败，请重新检测或重新解压完整测试包。"); }
        return 0;
      }
#endif
      if (HIWORD(w_param) == EN_CHANGE &&
          (LOWORD(w_param) == kUserSnEditId ||
           LOWORD(w_param) == kApiKeyEditId ||
           LOWORD(w_param) == kUserPinEditId)) {
        update_save_button(*state);
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
        state->ready = false;
        ::EnableWindow(state->launch, FALSE);
#endif
        return 0;
      }
      if (LOWORD(w_param) == kSaveButtonId &&
          HIWORD(w_param) == BN_CLICKED) {
        save(*state);
        return 0;
      }
      break;

    case WM_CLOSE:
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
      if (state && state->checking.load()) {
        ::SetWindowTextW(state->status, state->installing ? L"安装任务进行中，请先完成或退出安装器。" : L"检测正在访问安全卡，请等待检测结束后关闭。");
        return 0;
      }
#endif
      if (state != nullptr) clear_fields(*state);
      static_cast<void>(::DestroyWindow(window));
      return 0;

    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return ::DefWindowProcW(window, message, w_param, l_param);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  static_cast<void>(::SetProcessDpiAwarenessContext(
      DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));

  WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .style = CS_HREDRAW | CS_VREDRAW,
      .lpfnWndProc = window_procedure,
      .hInstance = instance,
      .hIcon = ::LoadIconW(nullptr, IDI_APPLICATION),
      .hCursor = ::LoadCursorW(nullptr, IDC_ARROW),
      .hbrBackground = reinterpret_cast<HBRUSH>(
          static_cast<INT_PTR>(COLOR_WINDOW + 1)),
      .lpszClassName = kWindowClass,
      .hIconSm = ::LoadIconW(nullptr, IDI_APPLICATION),
  };
  if (::RegisterClassExW(&window_class) == 0) return 1;

  UiState state;
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                      WS_MINIMIZEBOX;
  RECT window_rectangle{0, 0, 620, 350};
#ifdef USB_VAULT_CONFIG_DIAGNOSTICS
  window_rectangle.bottom = 660;
#endif
  if (::AdjustWindowRectEx(&window_rectangle, style, FALSE, 0) == FALSE) {
    return 1;
  }
  const HWND window = ::CreateWindowExW(
      0, kWindowClass, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
      window_rectangle.right - window_rectangle.left,
      window_rectangle.bottom - window_rectangle.top, nullptr, nullptr,
      instance, &state);
  if (window == nullptr) return 1;

  ::ShowWindow(window, show_command);
  static_cast<void>(::UpdateWindow(window));
  MSG message{};
  for (;;) {
    const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
    if (result == -1) return 1;
    if (result == 0) break;
    if (::IsDialogMessageW(window, &message) == FALSE) {
      static_cast<void>(::TranslateMessage(&message));
      static_cast<void>(::DispatchMessageW(&message));
    }
  }
  return static_cast<int>(message.wParam);
}
