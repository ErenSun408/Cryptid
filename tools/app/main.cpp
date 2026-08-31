#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP) || \
    defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
#include <shellapi.h>
#endif

#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_card_enrollment.h"
#include "platform/windows/hid_card_device_notifications.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_device_lifecycle_controller.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/offline_card_password_policy.h"
#include "usb_vault/ui/card_product_ui_session_controller.h"
#include "usb_vault/ui/card_product_webview_host_object.h"
#else
#include "usb_vault/application/application_session.h"
#endif
#include "usb_vault/application/container_layout_v1.h"
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
#include "card/vikey/vikey_card_store.h"
#include "card/vikey/vikey_development_app_config.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_blob_backend.h"
#include "platform/windows/hid_card_device_notifications.h"
#else
#if !defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
#include "usb_vault/card/fixed_blob_card_store.h"
#endif
#endif
#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/ui/webview_host_object.h"
#include "usb_vault/ui/webview_session_event.h"
#include "usb_vault/volume/volume_provider.h"

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"UsbVaultMainWindow";
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP) || \
    defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
constexpr wchar_t kWindowTitle[] =
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
    L"USB Vault - P2 ViKey 联网开发模式";
#else
    L"USB Vault - ViKey 联网开发模式";
#endif
constexpr usb_vault::platform::windows::HidDeviceIdentity
    kDevelopmentCardIdentity{.vendor_id = 0x1987, .product_id = 0x0331};
#else
constexpr wchar_t kWindowTitle[] = L"USB Vault";
#endif
constexpr wchar_t kHostObjectName[] = L"nativeBridge";
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
std::wstring g_page_uri;
std::unique_ptr<usb_vault::volume::VolumeProvider> g_volume_provider;
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
std::unique_ptr<usb_vault::card::vikey::VikeyHelperCardEnrollment>
    g_card_enrollment;
std::unique_ptr<usb_vault::application::CardEntryContextController>
    g_card_entry;
std::unique_ptr<usb_vault::application::CardAuthenticationSession>
    g_authentication;
std::unique_ptr<usb_vault::application::CardLoginMountSession>
    g_card_volume_session;
std::unique_ptr<usb_vault::security::OfflineCardPasswordPolicy>
    g_password_policy;
std::unique_ptr<usb_vault::ui::CardProductUiSessionController> g_ui_session;
#else
std::unique_ptr<usb_vault::application::IApplicationSessionActions>
    g_application_session;
std::unique_ptr<usb_vault::ui::UiSessionController> g_ui_session;
std::unique_ptr<usb_vault::card::ICardStore> g_card_store;
#endif
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
std::unique_ptr<
    usb_vault::card::vikey::development::WindowsCredentialProvider>
    g_vikey_credential_provider;
std::unique_ptr<usb_vault::card::vikey::VikeyHelperBlobBackend>
    g_vikey_backend;
std::unique_ptr<usb_vault::application::CardPresenceController>
    g_card_presence_controller;
std::unique_ptr<
    usb_vault::platform::windows::HidCardDeviceNotificationRegistration>
    g_card_device_notifications;
#elif defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
std::unique_ptr<
    usb_vault::card::vikey::development::WindowsCredentialProvider>
    g_vikey_credential_provider;
std::unique_ptr<usb_vault::application::CardPresenceController>
    g_card_presence_controller;
std::unique_ptr<usb_vault::application::CardDeviceLifecycleController>
    g_card_lifecycle_controller;
std::unique_ptr<
    usb_vault::platform::windows::HidCardDeviceNotificationRegistration>
    g_card_device_notifications;
#endif

void show_failure(const wchar_t* const message) noexcept {
  static_cast<void>(::MessageBoxW(nullptr, message, kWindowTitle,
                                  MB_OK | MB_ICONERROR));
}

void show_warning(const wchar_t* const message) noexcept {
  static_cast<void>(::MessageBoxW(nullptr, message, kWindowTitle,
                                  MB_OK | MB_ICONWARNING));
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
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
      child_path(executable_directory(), L"card_session.html");
#else
      child_path(executable_directory(), L"index.html");
#endif

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

#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP) || \
    defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
class LocalArguments final {
 public:
  LocalArguments() {
    values_ = ::CommandLineToArgvW(::GetCommandLineW(), &count_);
    if (values_ == nullptr || count_ <= 0) {
      throw std::runtime_error("cannot parse development command line");
    }
  }

  ~LocalArguments() noexcept {
    if (values_ != nullptr) static_cast<void>(::LocalFree(values_));
  }

  LocalArguments(const LocalArguments&) = delete;
  LocalArguments& operator=(const LocalArguments&) = delete;

  [[nodiscard]] std::vector<std::wstring_view> views() const {
    std::vector<std::wstring_view> result;
    result.reserve(static_cast<std::size_t>(count_));
    for (int index = 0; index < count_; ++index) {
      result.emplace_back(values_[index]);
    }
    return result;
  }

 private:
  LPWSTR* values_{};
  int count_{};
};
#endif

#if !defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
void initialize_card_store() {
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
  const LocalArguments arguments;
  const std::vector<std::wstring_view> views = arguments.views();
  auto provider = std::make_unique<
      usb_vault::card::vikey::development::WindowsCredentialProvider>();
  auto backend =
      std::make_unique<usb_vault::card::vikey::VikeyHelperBlobBackend>(
          usb_vault::card::vikey::development::parse_app_config(views),
          *provider);
  auto store = std::make_unique<usb_vault::card::vikey::VikeyCardStore>(
      *backend);
  g_vikey_credential_provider = std::move(provider);
  g_vikey_backend = std::move(backend);
  g_card_store = std::move(store);
#else
  g_card_store =
      std::make_unique<usb_vault::card::FixedBlobCardStore>();
#endif
}
#endif

void initialize_application_session([[maybe_unused]] const HWND window) {
#if !defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
  initialize_card_store();
#endif
  const std::wstring directory = executable_directory();
  const auto drives = unused_drive_letters();
  namespace layout_v1 = usb_vault::application;
  usb_vault::application::ContainerLayout layout{
      .container_a = {.path = child_path(directory,
                                         layout_v1::kContainerAFilename),
                      .salt = layout_v1::kContainerASalt,
                      .drive_letter = drives[0]},
      .container_b = {.path = child_path(directory,
                                         layout_v1::kContainerBFilename),
                      .salt = layout_v1::kContainerBSalt,
                      .drive_letter = drives[1]},
  };
  g_volume_provider =
      std::make_unique<usb_vault::volume::VolumeProvider>();
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
  const LocalArguments arguments;
  const std::vector<std::wstring_view> views = arguments.views();
  auto credential_provider = std::make_unique<
      usb_vault::card::vikey::development::WindowsCredentialProvider>();
  auto enrollment = std::make_unique<
      usb_vault::card::vikey::VikeyHelperCardEnrollment>(
          usb_vault::card::vikey::development::parse_app_config(views),
          *credential_provider);
  auto entry = std::make_unique<
      usb_vault::application::CardEntryContextController>(*enrollment);
  auto authentication = std::make_unique<
      usb_vault::application::CardAuthenticationSession>();
  auto volume_session = std::make_unique<
      usb_vault::application::CardLoginMountSession>(
          *authentication, *g_volume_provider, std::move(layout),
          child_path(directory, L"usb_vault_volume_creator.exe"));
  auto password_policy = std::make_unique<
      usb_vault::security::OfflineCardPasswordPolicy>();
  auto ui_session = std::make_unique<
      usb_vault::ui::CardProductUiSessionController>(
          *entry, *authentication, *volume_session, *password_policy);
  auto presence =
      std::make_unique<usb_vault::application::CardPresenceController>(
          *volume_session);
  auto lifecycle = std::make_unique<
      usb_vault::application::CardDeviceLifecycleController>(
          *presence, *entry, *authentication);
  auto notifications = std::make_unique<
      usb_vault::platform::windows::HidCardDeviceNotificationRegistration>(
      window, kDevelopmentCardIdentity);
  if (notifications->target_present()) {
    static_cast<void>(lifecycle->handle_arrival());
  }
  g_vikey_credential_provider = std::move(credential_provider);
  g_card_enrollment = std::move(enrollment);
  g_card_entry = std::move(entry);
  g_authentication = std::move(authentication);
  g_card_volume_session = std::move(volume_session);
  g_password_policy = std::move(password_policy);
  g_ui_session = std::move(ui_session);
  g_card_presence_controller = std::move(presence);
  g_card_lifecycle_controller = std::move(lifecycle);
  g_card_device_notifications = std::move(notifications);
#else
  g_application_session =
      std::make_unique<usb_vault::application::ApplicationSession>(
          *g_card_store, *g_volume_provider, std::move(layout));
  g_ui_session = std::make_unique<usb_vault::ui::UiSessionController>(
      *g_application_session);
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
  auto presence =
      std::make_unique<usb_vault::application::CardPresenceController>(
          *g_application_session);
  auto notifications = std::make_unique<
      usb_vault::platform::windows::HidCardDeviceNotificationRegistration>(
      window, kDevelopmentCardIdentity);
  if (notifications->target_present()) {
    static_cast<void>(presence->handle_arrival());
  }
  g_card_presence_controller = std::move(presence);
  g_card_device_notifications = std::move(notifications);
#endif
#endif
}

void clear_application_session() noexcept {
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
  g_card_device_notifications.reset();
  g_card_lifecycle_controller.reset();
  g_card_presence_controller.reset();
  g_ui_session.reset();
  g_password_policy.reset();
  g_card_volume_session.reset();
  g_authentication.reset();
  g_card_entry.reset();
  g_card_enrollment.reset();
  g_volume_provider.reset();
  g_vikey_credential_provider.reset();
#else
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
  g_card_device_notifications.reset();
  g_card_presence_controller.reset();
#endif
  g_ui_session.reset();
  g_application_session.reset();
  g_volume_provider.reset();
  g_card_store.reset();
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP)
  g_vikey_backend.reset();
  g_vikey_credential_provider.reset();
#endif
#endif
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

void notify_webview_session_changed() noexcept {
  if (!g_webview) return;
  const HRESULT result = g_webview->ExecuteScript(
      usb_vault::ui::kWebviewSessionChangedScript,
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [](const HRESULT script_result, LPCWSTR) -> HRESULT {
            if (FAILED(script_result)) {
              show_failure(
                  L"安全会话状态已变化，但界面刷新失败。请重新打开应用确认状态。");
            }
            return S_OK;
          })
          .Get());
  if (FAILED(result)) {
    show_failure(
        L"安全会话状态已变化，但界面刷新失败。请重新打开应用确认状态。");
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
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
                      current = usb_vault::ui::
                          create_card_product_webview_host_object(
                              *g_ui_session, &host_object);
#else
                      current = usb_vault::ui::create_webview_host_object(
                          *g_ui_session, &host_object);
#endif
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
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP) || \
    defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
    case WM_DEVICECHANGE:
      if (g_card_device_notifications != nullptr &&
#if defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
          g_card_lifecycle_controller != nullptr) {
        const auto transition =
            usb_vault::platform::windows::handle_hid_device_change(
                *g_card_lifecycle_controller, wparam, lparam,
                kDevelopmentCardIdentity);
        using Transition = usb_vault::application::CardPresenceTransition;
        if (transition.entry_changed ||
            transition.presence != Transition::no_change) {
          notify_webview_session_changed();
        }
        if (transition.presence ==
            Transition::active_session_force_unmounted) {
          show_warning(
              L"安全卡已拔出，磁盘正在被占用，已强制卸载。"
              L"未写入磁盘的数据可能已丢失。");
        }
        if (transition.presence == Transition::active_session_remains) {
          show_failure(
              L"安全卡已拔出，强制卸载当前磁盘仍然失败。"
              L"请立即关闭占用该磁盘的程序并重试安全卸载。");
        }
#else
          g_card_presence_controller != nullptr) {
        const auto transition =
            usb_vault::platform::windows::handle_hid_device_change(
                *g_card_presence_controller, wparam, lparam,
                kDevelopmentCardIdentity);
        using Transition = usb_vault::application::CardPresenceTransition;
        if (transition == Transition::active_session_unmounted ||
            transition == Transition::active_session_force_unmounted ||
            transition == Transition::active_session_remains) {
          notify_webview_session_changed();
        }
        if (transition == Transition::active_session_force_unmounted) {
          show_warning(
              L"安全卡已拔出，磁盘正在被占用，已强制卸载。"
              L"未写入磁盘的数据可能已丢失。");
        }
        if (transition == Transition::active_session_remains) {
          show_failure(
              L"安全卡已拔出，强制卸载当前磁盘仍然失败。"
              L"请立即关闭占用该磁盘的程序并重试安全卸载。");
        }
#endif
      }
      return 0;
#endif
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
#if defined(USB_VAULT_VIKEY_DEVELOPMENT_APP) || \
    defined(USB_VAULT_VIKEY_P2_DEVELOPMENT_APP)
      g_card_device_notifications.reset();
#endif
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
    initialize_application_session(window);
    initialize_webview(window);
  } catch (...) {
    show_failure(L"本地界面初始化失败。");
    ::DestroyWindow(window);
    clear_application_session();
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

  clear_application_session();
  ::CoUninitialize();
  return static_cast<int>(message.wParam);
}
