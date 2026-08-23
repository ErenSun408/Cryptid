#include "usb_vault/ui/webview_host_object.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::UiSessionController;

constexpr wchar_t kWindowClass[] = L"UsbVaultTypedArrayIntegration";
constexpr wchar_t kHostObjectName[] = L"nativeBridge";
constexpr wchar_t kPageName[] = L"webview_session_integration.html";
constexpr UINT_PTR kTimeoutTimer = 1;
constexpr UINT kTimeoutMilliseconds = 15000;
constexpr int kInitializationFailure = 11;
constexpr int kSubmissionFailure = 12;
constexpr int kUnmountFailure = 13;
constexpr int kDriverLoadedWithoutResult = 14;
constexpr int kSubmissionPayloadMismatch = 15;
constexpr int kSubmissionStateMismatch = 16;

class ProbeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer password) override {
    submission_observed = true;
    constexpr std::array<std::byte, 4> expected{
        std::byte{0x70}, std::byte{0x00}, std::byte{0xC3}, std::byte{0xA4}};
    if (password.size() != expected.size()) {
      throw std::runtime_error("typed array size changed");
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
      if (password.bytes()[index] != expected[index]) {
        throw std::runtime_error("typed array bytes changed");
      }
    }
    password.clear();
    payload_matched = true;
    received = true;
    is_mounted = true;
  }

  void unmount_active() override {
    if (!is_mounted) {
      throw std::runtime_error("unmount called without active session");
    }
    is_mounted = false;
    unmounted = true;
  }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  bool received{false};
  bool submission_observed{false};
  bool payload_matched{false};
  bool is_mounted{false};
  bool unmounted{false};
};

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
ProbeSession g_probe_session;
UiSessionController g_ui_session(g_probe_session);
std::wstring g_page_uri;
int g_exit_code = 1;

[[nodiscard]] std::wstring page_uri() {
  std::vector<wchar_t> executable_path(32768);
  const DWORD length = ::GetModuleFileNameW(
      nullptr, executable_path.data(),
      static_cast<DWORD>(executable_path.size()));
  if (length == 0 || length >= executable_path.size() ||
      !::PathRemoveFileSpecW(executable_path.data())) {
    throw std::runtime_error("cannot resolve integration executable path");
  }

  std::vector<wchar_t> page_path(32768);
  if (!::PathCombineW(page_path.data(), executable_path.data(), kPageName)) {
    throw std::runtime_error("cannot resolve integration page path");
  }

  std::vector<wchar_t> uri(32768);
  DWORD uri_size = static_cast<DWORD>(uri.size());
  if (FAILED(::UrlCreateFromPathW(page_path.data(), uri.data(), &uri_size, 0))) {
    throw std::runtime_error("cannot create integration page URI");
  }
  return std::wstring(uri.data(), uri_size);
}

void fail_and_close(const HWND window) noexcept {
  g_exit_code = 1;
  static_cast<void>(::DestroyWindow(window));
}

void initialize_webview(const HWND window) {
  g_page_uri = page_uri();
  const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [window](const HRESULT environment_result,
                   ICoreWebView2Environment* const environment) -> HRESULT {
            if (FAILED(environment_result) || environment == nullptr) {
              fail_and_close(window);
              return FAILED(environment_result) ? environment_result : E_FAIL;
            }
            return environment->CreateCoreWebView2Controller(
                window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [window](const HRESULT controller_result,
                             ICoreWebView2Controller* const controller) -> HRESULT {
                      if (FAILED(controller_result) || controller == nullptr) {
                        fail_and_close(window);
                        return FAILED(controller_result) ? controller_result
                                                         : E_FAIL;
                      }
                      g_controller = controller;
                      HRESULT current = g_controller->get_CoreWebView2(&g_webview);
                      if (FAILED(current) || !g_webview) {
                        fail_and_close(window);
                        return FAILED(current) ? current : E_FAIL;
                      }

                      ComPtr<IDispatch> host_object;
                      current = usb_vault::ui::create_webview_host_object(
                          g_ui_session, &host_object);
                      if (FAILED(current)) {
                        fail_and_close(window);
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
                        fail_and_close(window);
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
                        fail_and_close(window);
                        return current;
                      }

                      EventRegistrationToken message_token{};
                      current = g_webview->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [window](ICoreWebView2*,
                                       ICoreWebView2WebMessageReceivedEventArgs*
                                           args) -> HRESULT {
                                LPWSTR raw_message = nullptr;
                                const HRESULT message_result =
                                    args->TryGetWebMessageAsString(&raw_message);
                                if (SUCCEEDED(message_result) &&
                                    raw_message != nullptr) {
                                  if (std::wcscmp(raw_message,
                                                 L"driver-loaded") == 0) {
                                    g_exit_code = kDriverLoadedWithoutResult;
                                    ::CoTaskMemFree(raw_message);
                                    return S_OK;
                                  }
                                  if (std::wcscmp(raw_message, L"pass") == 0 &&
                                      g_probe_session.received &&
                                      g_probe_session.unmounted) {
                                    g_exit_code = 0;
                                  } else if (std::wcscmp(
                                                 raw_message,
                                                 L"fail-initialize") == 0) {
                                    g_exit_code = kInitializationFailure;
                                  } else if (std::wcscmp(raw_message,
                                                        L"fail-submit") == 0) {
                                    g_exit_code =
                                        !g_probe_session.submission_observed
                                            ? kSubmissionFailure
                                            : !g_probe_session.payload_matched
                                                  ? kSubmissionPayloadMismatch
                                                  : kSubmissionStateMismatch;
                                  } else if (std::wcscmp(raw_message,
                                                        L"fail-unmount") == 0) {
                                    g_exit_code = kUnmountFailure;
                                  }
                                }
                                ::CoTaskMemFree(raw_message);
                                static_cast<void>(::DestroyWindow(window));
                                return message_result;
                              })
                              .Get(),
                          &message_token);
                      if (FAILED(current)) {
                        fail_and_close(window);
                        return current;
                      }
                      return g_webview->Navigate(g_page_uri.c_str());
                    })
                    .Get());
          })
          .Get());
  if (FAILED(result)) {
    throw std::runtime_error("cannot start typed array integration WebView2");
  }
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_TIMER:
      if (wparam == kTimeoutTimer) {
        if (g_exit_code == kDriverLoadedWithoutResult) {
          static_cast<void>(::DestroyWindow(window));
        } else {
          fail_and_close(window);
        }
        return 0;
      }
      break;
    case WM_DESTROY:
      static_cast<void>(::KillTimer(window, kTimeoutTimer));
      g_webview.Reset();
      g_controller.Reset();
      g_host_object.Reset();
      ::PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int) {
  const HRESULT com_result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    return 1;
  }

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&window_class) == 0) {
    ::CoUninitialize();
    return 1;
  }

  const HWND window = ::CreateWindowExW(
      0, kWindowClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
      nullptr, instance, nullptr);
  if (window == nullptr) {
    ::CoUninitialize();
    return 1;
  }

  try {
    if (::SetTimer(window, kTimeoutTimer, kTimeoutMilliseconds, nullptr) == 0) {
      throw std::runtime_error("cannot start integration timeout");
    }
    initialize_webview(window);
  } catch (...) {
    static_cast<void>(::DestroyWindow(window));
  }

  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
  ::CoUninitialize();
  return g_exit_code;
}
