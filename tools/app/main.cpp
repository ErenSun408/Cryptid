#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

#include "usb_vault/application/application_session.h"
#include "usb_vault/card/fixed_blob_card_store.h"
#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/ui/webview_host_object.h"
#include "usb_vault/volume/volume_provider.h"

#include <array>
#include <cstddef>
#include <memory>
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
constexpr usb_vault::security::KdfV1Salt kContainerASalt{
    std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
    std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
    std::byte{0x38}, std::byte{0x39}, std::byte{0x3A}, std::byte{0x3B},
    std::byte{0x3C}, std::byte{0x3D}, std::byte{0x3E}, std::byte{0x3F}};
constexpr usb_vault::security::KdfV1Salt kContainerBSalt{
    std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
    std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
    std::byte{0x28}, std::byte{0x29}, std::byte{0x2A}, std::byte{0x2B},
    std::byte{0x2C}, std::byte{0x2D}, std::byte{0x2E}, std::byte{0x2F}};

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
std::wstring g_page_uri;
usb_vault::card::FixedBlobCardStore g_card_store;
std::unique_ptr<usb_vault::volume::VolumeProvider> g_volume_provider;
std::unique_ptr<usb_vault::application::IApplicationSessionActions>
    g_application_session;
std::unique_ptr<usb_vault::ui::UiSessionController> g_ui_session;

void show_failure(const wchar_t* const message) noexcept {
  static_cast<void>(::MessageBoxW(nullptr, message, kWindowTitle,
                                  MB_OK | MB_ICONERROR));
}

[[nodiscard]] std::wstring executable_directory() {
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
  return executable_path.data();
}

[[nodiscard]] std::wstring child_path(const std::wstring& directory,
                                      const wchar_t* const name) {
  std::vector<wchar_t> page_path(32768);
  if (!::PathCombineW(page_path.data(), directory.c_str(), name)) {
    throw std::runtime_error("cannot resolve application child path");
  }
  return page_path.data();
}

[[nodiscard]] std::wstring local_page_uri() {
  const std::wstring page_path =
      child_path(executable_directory(), L"index.html");

  std::vector<wchar_t> uri(32768);
  DWORD uri_size = static_cast<DWORD>(uri.size());
  const HRESULT result =
      ::UrlCreateFromPathW(page_path.c_str(), uri.data(), &uri_size, 0);
  if (FAILED(result)) {
    throw std::runtime_error("cannot create local UI URI");
  }
  return std::wstring(uri.data(), uri_size);
}

[[nodiscard]] std::array<wchar_t, 2> unused_drive_letters() {
  const DWORD drives = ::GetLogicalDrives();
  if (drives == 0) {
    throw std::runtime_error("cannot inspect available drive letters");
  }
  std::array<wchar_t, 2> result{};
  std::size_t count = 0;
  for (wchar_t letter = L'Z'; letter >= L'D' && count < result.size();
       --letter) {
    const DWORD mask = 1UL << static_cast<DWORD>(letter - L'A');
    if ((drives & mask) == 0) {
      result[count++] = letter;
    }
  }
  if (count != result.size()) {
    throw std::runtime_error("two unused drive letters are required");
  }
  return result;
}

void initialize_application_session() {
  const std::wstring directory = executable_directory();
  const auto drives = unused_drive_letters();
  usb_vault::application::ContainerLayout layout{
      .container_a = {.path = child_path(directory, L"container_a.hc"),
                      .salt = kContainerASalt,
                      .drive_letter = drives[0]},
      .container_b = {.path = child_path(directory, L"container_b.hc"),
                      .salt = kContainerBSalt,
                      .drive_letter = drives[1]},
  };
  g_volume_provider =
      std::make_unique<usb_vault::volume::VolumeProvider>();
  g_application_session =
      std::make_unique<usb_vault::application::ApplicationSession>(
          g_card_store, *g_volume_provider, std::move(layout));
  g_ui_session = std::make_unique<usb_vault::ui::UiSessionController>(
      *g_application_session);
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
                      current = settings->put_AreDevToolsEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current =
                          settings->put_AreDefaultContextMenusEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current =
                          settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current = settings->put_IsWebMessageEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current = settings->put_IsStatusBarEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }

                      ComPtr<ICoreWebView2Settings4> restricted_settings;
                      current = settings.As(&restricted_settings);
                      if (FAILED(current)) {
                        return current;
                      }
                      current = restricted_settings
                                    ->put_AreBrowserAcceleratorKeysEnabled(
                                        FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current = restricted_settings
                                    ->put_IsPasswordAutosaveEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }
                      current = restricted_settings
                                    ->put_IsGeneralAutofillEnabled(FALSE);
                      if (FAILED(current)) {
                        return current;
                      }

                      ComPtr<IDispatch> host_object;
                      current = usb_vault::ui::create_webview_host_object(
                          *g_ui_session, &host_object);
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
    case WM_CLOSE:
      if (g_ui_session != nullptr &&
          !usb_vault::ui::prepare_ui_session_for_close(*g_ui_session)) {
        show_failure(L"无法安全卸载当前磁盘，请关闭占用文件后重试。");
        return 0;
      }
      static_cast<void>(::DestroyWindow(window));
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
    initialize_application_session();
    initialize_webview(window);
  } catch (...) {
    show_failure(L"本地界面初始化失败。");
    ::DestroyWindow(window);
    g_ui_session.reset();
    g_application_session.reset();
    g_volume_provider.reset();
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

  g_ui_session.reset();
  g_application_session.reset();
  g_volume_provider.reset();
  ::CoUninitialize();
  return static_cast<int>(message.wParam);
}
