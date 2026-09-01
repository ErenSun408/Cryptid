#include "usb_vault/ui/card_product_webview_host_object.h"
#include "usb_vault/ui/webview_session_event.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/offline_card_password_policy.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

#include <cstddef>
#include <cwchar>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ContainerAStatus;
using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::ConfirmedPassword;
using usb_vault::security::OfflineCardPasswordPolicy;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;

constexpr wchar_t kWindowClass[] = L"UsbVaultCardProductIntegration";
constexpr wchar_t kHostObjectName[] = L"nativeBridge";
constexpr wchar_t kPageName[] = L"card_product_webview_integration.html";
constexpr UINT_PTR kTimeoutTimer = 1;
constexpr UINT kTimeoutMilliseconds = 20000;

[[nodiscard]] bool equals(const std::span<const std::byte> left,
                          const std::span<const std::byte> right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

[[nodiscard]] SecureBuffer root_secret() {
  SecureBuffer result(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

class FakeVolumeSession final : public IAuthenticatedCardVolumeSession {
 public:
  explicit FakeVolumeSession(CardAuthenticationSession& authentication)
      : authentication_(authentication) {}

  void create_container_a(const std::uint64_t size_bytes) override {
    created_a_size_bytes = size_bytes;
    ++create_a_calls;
    if (fail_next_create_a) {
      fail_next_create_a = false;
      throw std::runtime_error("injected container A creation failure");
    }
    container_a_exists = true;
    container_a_mounted = true;
    mounted_state = true;
  }

  std::uint32_t create_container(const std::wstring_view path,
                                 const std::uint64_t size_bytes) override {
    ++create_calls;
    created_size_bytes = size_bytes;
    if (fail_next_create) {
      fail_next_create = false;
      throw std::runtime_error("injected container creation failure");
    }
    return adopt(path, size_bytes);
  }

  std::uint32_t mount_container(const std::wstring_view path) override {
    ++mount_calls;
    for (Adopted& entry : containers) {
      if (entry.path != path) continue;
      entry.mounted = true;
      mounted_state = true;
      return entry.id;
    }
    return adopt(path, 64ULL * 1024ULL * 1024ULL);
  }

  void unmount_container(const std::uint32_t id) override {
    ++unmount_container_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = false;
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    std::vector<usb_vault::application::ContainerStatus> result;
    result.reserve(containers.size());
    for (const Adopted& entry : containers) {
      result.push_back({.id = entry.id,
                        .name = entry.name,
                        .mounted = entry.mounted,
                        .size_bytes = entry.size_bytes,
                        .drive_letter =
                            entry.mounted ? entry.drive_letter : wchar_t{}});
    }
    return result;
  }

  struct Adopted final {
    std::uint32_t id{};
    std::wstring path;
    std::wstring name;
    std::uint64_t size_bytes{};
    wchar_t drive_letter{};
    bool mounted{};
  };

  std::uint32_t adopt(const std::wstring_view path,
                      const std::uint64_t size_bytes) {
    const std::size_t separator = path.find_last_of(L"/");
    const std::wstring_view name = separator == std::wstring_view::npos
                                       ? path
                                       : path.substr(separator + 1);
    const std::uint32_t id = next_id++;
    containers.push_back(
        {.id = id,
         .path = std::wstring(path),
         .name = std::wstring(name),
         .size_bytes = size_bytes,
         .drive_letter = static_cast<wchar_t>(
             L'D' + static_cast<wchar_t>(containers.size())),
         .mounted = true});
    mounted_state = true;
    return id;
  }

  std::vector<Adopted> containers;
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  bool fail_next_create{true};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};

  [[nodiscard]] ContainerAStatus container_a_status() override {
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_exists ? created_a_size_bytes : 0,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  void consume_authenticated_login(
      SecureBuffer root, SecureBuffer login_password_utf8) override {
    static constexpr std::byte kExpected[]{
        std::byte{0x70}, std::byte{0x00}, std::byte{0xC3}, std::byte{0xA4}};
    login_payload_matched = equals(login_password_utf8.bytes(), kExpected);
    authentication_.establish(std::move(root));
    login_password_utf8.clear();
    container_a_mounted = container_a_exists;
    mounted_state = true;
    ++consume_calls;
  }

  void unmount_active() override {
    if (!mounted_state) {
      throw std::runtime_error("unmount called without a fake volume");
    }
    container_a_mounted = false;
    mounted_state = false;
    ++unmount_calls;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("forced cleanup is outside this UI contract");
  }

  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  CardAuthenticationSession& authentication_;
  bool mounted_state{};
  bool login_payload_matched{};
  bool fail_next_create_a{true};
  bool container_a_exists{};
  bool container_a_mounted{};
  std::uint64_t created_a_size_bytes{};
  int create_a_calls{};
  int consume_calls{};
  int unmount_calls{};
};

class FakeEntry final : public ICardEntryActions {
 public:
  void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      ConfirmedPassword confirmed_daily_password) override {
    static constexpr std::byte kExpected[]{
        std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'},
        std::byte{'e'}, std::byte{'f'}, std::byte{'g'}, std::byte{'h'},
        std::byte{'i'}, std::byte{'j'}, std::byte{'k'}, std::byte{0xC3},
        std::byte{0xA9}};
    SecureBuffer password = std::move(confirmed_daily_password).take();
    initialization_payload_matched = equals(password.bytes(), kExpected);
    password.clear();
    route = CardEntryRoute::unavailable;
    authentication.establish(root_secret());
    ++initialization_calls;
  }

  void authenticate_login(
      usb_vault::application::ICardLoginSuccessConsumer& success_consumer,
      SecureBuffer login_password_utf8) override {
    static constexpr std::byte kRejected[]{
        std::byte{'w'}, std::byte{'r'}, std::byte{'o'}, std::byte{'n'},
        std::byte{'g'}, std::byte{'-'}, std::byte{'p'}, std::byte{'a'},
        std::byte{'s'}, std::byte{'s'}, std::byte{'w'}, std::byte{'o'},
        std::byte{'r'}, std::byte{'d'}};
    ++login_calls;
    if (equals(login_password_utf8.bytes(), kRejected)) {
      ++credential_rejections;
      throw usb_vault::card::CardCredentialRejected();
    }
    route = CardEntryRoute::unavailable;
    success_consumer.consume_authenticated_login(
        root_secret(), std::move(login_password_utf8));
  }

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }

  std::optional<CardEntryRoute> route;
  bool initialization_payload_matched{};
  int initialization_calls{};
  int login_calls{};
  int credential_rejections{};
};

// The fixture drives the page, never a real dialog: the chooser hands back
// whatever path the current scenario expects, or an empty string to stand for
// a user who cancelled.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  // The second selection is cancelled so the page has to prove it treats a
  // cancelled dialog as an ordinary outcome rather than a failure.
  [[nodiscard]] std::wstring choose_new_container() override {
    ++new_calls;
    return new_calls == 2 ? std::wstring{} : new_path;
  }

  [[nodiscard]] std::wstring choose_existing_container() override {
    ++existing_calls;
    return existing_path;
  }

  std::wstring new_path{L"D:/Vault/integration.hc"};
  std::wstring existing_path;
  int new_calls{};
  int existing_calls{};
};

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
FakeEntry g_entry;
CardAuthenticationSession g_authentication;
FakeVolumeSession g_volume(g_authentication);
OfflineCardPasswordPolicy g_policy;
FakeFileChooser g_file_chooser;
CardProductUiSessionController g_ui_session(
    g_entry, g_authentication, g_volume, g_policy, g_file_chooser);
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
    throw std::runtime_error("cannot resolve product integration page path");
  }

  std::vector<wchar_t> uri(32768);
  DWORD uri_size = static_cast<DWORD>(uri.size());
  if (FAILED(::UrlCreateFromPathW(page_path.data(), uri.data(), &uri_size, 0))) {
    throw std::runtime_error("cannot create product integration page URI");
  }
  return std::wstring(uri.data(), uri_size);
}

void fail_and_close(const HWND window) noexcept {
  g_exit_code = 1;
  static_cast<void>(::DestroyWindow(window));
}

[[nodiscard]] HRESULT dispatch_session_change(const HWND window) noexcept {
  const HRESULT result = g_webview->ExecuteScript(
      usb_vault::ui::kWebviewSessionChangedScript, nullptr);
  if (FAILED(result)) fail_and_close(window);
  return result;
}

[[nodiscard]] bool product_flow_matched() noexcept {
  return g_entry.initialization_calls == 1 &&
         g_entry.initialization_payload_matched &&
         g_entry.login_calls == 2 && g_entry.credential_rejections == 1 &&
         g_volume.create_calls == 2 && g_file_chooser.new_calls == 3 &&
         g_volume.containers.size() == 1 &&
         g_volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL &&
         g_volume.consume_calls == 1 && g_volume.login_payload_matched &&
         g_volume.unmount_calls == 1 && !g_volume.mounted();
}

[[nodiscard]] int failure_exit_code(const wchar_t* const message) noexcept {
  constexpr const wchar_t* failures[]{
      L"fail-no-card",
      L"fail-initialization-route",
      L"fail-short-password-state",
      L"fail-password-visibility-persistence",
      L"fail-confirmation-visibility-persistence",
      L"fail-strength",
      L"fail-initialization-mismatch",
      L"fail-initialization-error-clear",
      L"fail-initialization-submit",
      L"fail-dashboard-navigation",
      L"fail-creation-entry",
      L"fail-creation-validation",
      L"fail-creation-failure-feedback",
      L"fail-creation-cancelled",
      L"fail-creation-submit",
      L"fail-creation-close",
      L"fail-login-route",
      L"fail-external-input-clear",
      L"fail-login-retry",
      L"fail-login-success",
      L"fail-unavailable",
      L"fail-unmount",
      L"fail-final-no-card",
      L"fail-driver-exception",
  };
  int code = 10;
  for (const wchar_t* const failure : failures) {
    if (std::wcscmp(message, failure) == 0) return code;
    ++code;
  }
  return 2;
}

HRESULT handle_message(const HWND window,
                       ICoreWebView2WebMessageReceivedEventArgs& args) {
  LPWSTR raw_message = nullptr;
  const HRESULT message_result = args.TryGetWebMessageAsString(&raw_message);
  if (FAILED(message_result) || raw_message == nullptr) {
    ::CoTaskMemFree(raw_message);
    fail_and_close(window);
    return FAILED(message_result) ? message_result : E_FAIL;
  }

  const auto finish = [raw_message](const HRESULT result) noexcept {
    ::CoTaskMemFree(raw_message);
    return result;
  };
  if (std::wcscmp(raw_message, L"driver-loaded") == 0) {
    return finish(S_OK);
  }
  if (std::wcscmp(raw_message, L"request-initialization") == 0) {
    g_authentication.clear_authentication();
    g_volume.container_a_mounted = false;
    g_volume.mounted_state = false;
    g_entry.route = CardEntryRoute::initialization_required;
    return finish(dispatch_session_change(window));
  }
  if (std::wcscmp(raw_message, L"request-login") == 0) {
    g_authentication.clear_authentication();
    g_volume.container_a_mounted = false;
    g_volume.mounted_state = false;
    g_entry.route = CardEntryRoute::login_required;
    return finish(dispatch_session_change(window));
  }
  if (std::wcscmp(raw_message, L"request-residual-mount") == 0) {
    g_authentication.clear_authentication();
    g_entry.route = CardEntryRoute::unavailable;
    return finish(dispatch_session_change(window));
  }
  if (std::wcscmp(raw_message, L"request-no-card") == 0) {
    g_authentication.clear_authentication();
    g_volume.container_a_mounted = false;
    g_volume.mounted_state = false;
    g_entry.route.reset();
    return finish(dispatch_session_change(window));
  }

  if (std::wcscmp(raw_message, L"pass") == 0 && product_flow_matched()) {
    g_exit_code = 0;
  } else {
    g_exit_code = failure_exit_code(raw_message);
  }
  ::CoTaskMemFree(raw_message);
  static_cast<void>(::DestroyWindow(window));
  return S_OK;
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
                             ICoreWebView2Controller* const controller)
                        -> HRESULT {
                      if (FAILED(controller_result) || controller == nullptr) {
                        fail_and_close(window);
                        return FAILED(controller_result) ? controller_result
                                                         : E_FAIL;
                      }
                      g_controller = controller;
                      HRESULT current =
                          g_controller->get_CoreWebView2(&g_webview);
                      if (FAILED(current) || !g_webview) {
                        fail_and_close(window);
                        return FAILED(current) ? current : E_FAIL;
                      }

                      ComPtr<IDispatch> host_object;
                      current = usb_vault::ui::
                          create_card_product_webview_host_object(
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
                      current = g_webview->AddHostObjectToScript(
                          kHostObjectName, &host_value);
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
                                if (FAILED(uri_result)) return uri_result;
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
                              [window](
                                  ICoreWebView2*,
                                  ICoreWebView2WebMessageReceivedEventArgs* args)
                                  -> HRESULT {
                                if (args == nullptr) {
                                  fail_and_close(window);
                                  return E_POINTER;
                                }
                                return handle_message(window, *args);
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
    throw std::runtime_error("cannot start product integration WebView2");
  }
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_TIMER:
      if (wparam == kTimeoutTimer) {
        fail_and_close(window);
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
  if (FAILED(com_result)) return 1;

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&window_class) == 0) {
    ::CoUninitialize();
    return 1;
  }

  const HWND window = ::CreateWindowExW(
      0, kWindowClass, L"", WS_OVERLAPPEDWINDOW, 0, 0, 360, 320, nullptr,
      nullptr, instance, nullptr);
  if (window == nullptr) {
    ::CoUninitialize();
    return 1;
  }

  try {
    if (::SetTimer(window, kTimeoutTimer, kTimeoutMilliseconds, nullptr) == 0) {
      throw std::runtime_error("cannot start product integration timeout");
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
