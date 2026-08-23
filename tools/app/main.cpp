#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

#include "usb_vault/ui/webview_host_object.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"UsbVaultMainWindow";
constexpr wchar_t kWindowTitle[] = L"USB Vault";
constexpr wchar_t kHostObjectName[] = L"nativeBridge";

class BridgeUnavailableSession final
    : public usb_vault::application::IApplicationSessionActions {
 public:
  void authenticate_and_mount(
      usb_vault::security::SecureBuffer password_utf8) override {
    password_utf8.clear();
    throw std::runtime_error("application session is not configured");
  }

  void unmount_active() override {}
  [[nodiscard]] bool mounted() const noexcept override { return false; }
};

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
std::wstring g_page_uri;
BridgeUnavailableSession g_unavailable_session;
usb_vault::ui::UiSessionController g_ui_session(g_unavailable_session);

void show_failure(const wchar_t* const message) noexcept {
  static_cast<void>(::MessageBoxW(nullptr, message, kWindowTitle,
                                  MB_OK | MB_ICONERROR));
}

[[nodiscard]] std::wstring local_page_uri() {
  std::vector<wchar_t> executable_path(32768);
  const DWORD length = ::GetModuleFileNameW(
      nullptr, executable_path.data(),
      static_cast<DWORD>(executable_path.size()));
  if (length == 0 || length >= executable_path.size()) {
    throw std::runtime_error("cannot resolve application path");
  }
  if (!::PathRemoveFileSpecW(executable_path.data())) {
    throw std::runtime_error("cannot resolve application directory");
  }

  std::vector<wchar_t> page_path(32768);
  if (!::PathCombineW(page_path.data(), executable_path.data(),
                      L"index.html")) {
    throw std::runtime_error("cannot resolve local UI page");
  }

  std::vector<wchar_t> uri(32768);
  DWORD uri_size = static_cast<DWORD>(uri.size());
  const HRESULT result =
      ::UrlCreateFromPathW(page_path.data(), uri.data(), &uri_size, 0);
  if (FAILED(result)) {
    throw std::runtime_error("cannot create local UI URI");
  }
  return std::wstring(uri.data(), uri_size);
}

void resize_webview(const HWND window) noexcept {
  if (!g_controller) {
    return;
  }
  RECT bounds{};
  if (::GetClientRect(window, &bounds)) {
    static_cast<void>(g_controller->put_Bounds(bounds));
  }
}

void initialize_webview(const HWND window) {
  g_page_uri = local_page_uri();
  const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [window](const HRESULT environment_result,
                   ICoreWebView2Environment* const environment) -> HRESULT {
            if (FAILED(environment_result) || environment == nullptr) {
              show_failure(L"WebView2 运行时初始化失败。");
              return FAILED(environment_result) ? environment_result : E_FAIL;
            }
            return environment->CreateCoreWebView2Controller(
                window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [window](const HRESULT controller_result,
                             ICoreWebView2Controller* const controller) -> HRESULT {
                      if (FAILED(controller_result) || controller == nullptr) {
                        show_failure(L"WebView2 窗口初始化失败。");
                        return FAILED(controller_result) ? controller_result
                                                         : E_FAIL;
                      }
                      g_controller = controller;
                      HRESULT current = g_controller->get_CoreWebView2(&g_webview);
                      if (FAILED(current) || !g_webview) {
                        return FAILED(current) ? current : E_FAIL;
                      }

                      ComPtr<ICoreWebView2Settings> settings;
                      current = g_webview->get_Settings(&settings);
                      if (FAILED(current)) {
                        return current;
                      }
                      static_cast<void>(settings->put_AreDevToolsEnabled(FALSE));
                      static_cast<void>(settings->put_AreDefaultContextMenusEnabled(FALSE));
                      static_cast<void>(settings->put_IsStatusBarEnabled(FALSE));

                      ComPtr<IDispatch> host_object;
                      current = usb_vault::ui::create_webview_host_object(
                          g_ui_session, &host_object);
                      if (FAILED(current)) {
                        return current;
                      }
                      VARIANT host_value{};
                      ::VariantInit(&host_value);
                      host_value.vt = VT_DISPATCH;
                      host_value.pdispVal = host_object.Get();
                      host_value.pdispVal->AddRef();
                      current = g_webview->AddHostObjectToScript(kHostObjectName,
                                                                 &host_value);
                      static_cast<void>(::VariantClear(&host_value));
                      if (FAILED(current)) {
                        return current;
                      }
                      g_host_object = std::move(host_object);

                      EventRegistrationToken navigation_token{};
                      current = g_webview->add_NavigationStarting(
                          Callback<ICoreWebView2NavigationStartingEventHandler>(
                              [](ICoreWebView2*,
                                 ICoreWebView2NavigationStartingEventArgs* args)
                                  -> HRESULT {
                                LPWSTR raw_uri = nullptr;
                                const HRESULT uri_result = args->get_Uri(&raw_uri);
                                if (FAILED(uri_result)) {
                                  return uri_result;
                                }
                                const std::wstring uri = raw_uri ? raw_uri : L"";
                                ::CoTaskMemFree(raw_uri);
                                if (uri != g_page_uri) {
                                  static_cast<void>(args->put_Cancel(TRUE));
                                }
                                return S_OK;
                              })
                              .Get(),
                          &navigation_token);
                      if (FAILED(current)) {
                        return current;
                      }
                      resize_webview(window);
                      return g_webview->Navigate(g_page_uri.c_str());
                    })
                    .Get());
          })
          .Get());
  if (FAILED(result)) {
    throw std::runtime_error("cannot start WebView2 environment creation");
  }
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      resize_webview(window);
      return 0;
    case WM_DESTROY:
      g_webview.Reset();
      g_controller.Reset();
      g_host_object.Reset();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) {
  const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    show_failure(L"COM 初始化失败。");
    return 1;
  }

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&window_class) == 0) {
    show_failure(L"窗口类注册失败。");
    ::CoUninitialize();
    return 1;
  }

  const HWND window = ::CreateWindowExW(
      0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, 900, 640, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    show_failure(L"窗口创建失败。");
    ::CoUninitialize();
    return 1;
  }

  try {
    initialize_webview(window);
  } catch (...) {
    show_failure(L"本地界面初始化失败。");
    ::DestroyWindow(window);
    ::CoUninitialize();
    return 1;
  }

  ::ShowWindow(window, show_command);
  static_cast<void>(::UpdateWindow(window));
  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }

  ::CoUninitialize();
  return static_cast<int>(message.wParam);
}
