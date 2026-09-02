#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_session.h"

#include <Windows.h>

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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
    L"USB Vault - ViKey 开发凭据配置";

constexpr int kUserSnEditId = 1001;
constexpr int kApiKeyEditId = 1002;
constexpr int kUserPinEditId = 1003;
constexpr int kSaveButtonId = 1004;

struct UiState final {
  HWND user_sn{};
  HWND api_key{};
  HWND user_pin{};
  HWND save{};
  HWND status{};
  HFONT font{};
  bool saving{};
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
        L"已保存并复读验证。现在可以启动 USB Vault 开发程序。"));
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
      if (HIWORD(w_param) == EN_CHANGE &&
          (LOWORD(w_param) == kUserSnEditId ||
           LOWORD(w_param) == kApiKeyEditId ||
           LOWORD(w_param) == kUserPinEditId)) {
        update_save_button(*state);
        return 0;
      }
      if (LOWORD(w_param) == kSaveButtonId &&
          HIWORD(w_param) == BN_CLICKED) {
        save(*state);
        return 0;
      }
      break;

    case WM_CLOSE:
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
