#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <wrl.h>
#include <shellapi.h>

#include "card/vikey/vikey_development_app_config.h"
#include "card/verified_local_artifact.h"
#include "p2_deployment.h"
#include "usb_vault/application/product_settings_v1.h"
#include "card/vikey/vikey_development_credentials.h"
#include "card/vikey/vikey_helper_card_enrollment.h"
#include "platform/windows/hid_card_device_notifications.h"
#include "platform/windows/installation_state.h"
#include "platform/windows/shell_container_file_chooser.h"
#include "card/diagnostic_error.h"
#include "ui/webview_startup_state.h"
#include "platform/windows/vikey_configuration/diagnostics.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/card_device_lifecycle_controller.h"
#include "usb_vault/application/card_entry_context_controller.h"
#include "usb_vault/application/card_login_mount_session.h"
#include "usb_vault/security/offline_card_password_policy.h"
#include "usb_vault/ui/card_configuration_webview_host_object.h"
#include "usb_vault/ui/card_product_ui_session_controller.h"
#include "usb_vault/ui/card_product_webview_host_object.h"
#include "usb_vault/ui/external_drop_target.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/ui/ui_close_guard.h"
#include "usb_vault/ui/webview_session_event.h"
#include "usb_vault/volume/volume_provider.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"UsbVaultMainWindow";
constexpr wchar_t kWindowTitle[] = L"USB Vault";
constexpr usb_vault::platform::windows::HidDeviceIdentity
    kDevelopmentCardIdentity{.vendor_id = 0x1987, .product_id = 0x0331};
constexpr wchar_t kHostObjectName[] = L"nativeBridge";
HWND g_main_window{};
std::uint64_t g_window_generation{};
std::uint64_t g_page_generation{};
UINT64 g_navigation_id{};
bool g_navigation_complete{};
bool g_ready_poll_pending{};
usb_vault::ui::WebviewStartupState g_webview_startup;
// Which surface the one product window shows. Setup and the product session
// swap inside it rather than each owning a window, but the swap is still a
// full teardown: the card context, the bridge and the document are all gone
// before the other surface exists, so a credential change can never reach a
// session that was built from the previous values.
enum class StartupView { session, configuration };
StartupView g_view{StartupView::session};
// Set the moment a swap is accepted and cleared when it has been applied. The
// old surface keeps running until the posted message arrives, and it must not
// act on a card event in that window.
bool g_view_switch_pending{};
constexpr UINT kShowConfigurationMessage = WM_APP + 1;
constexpr UINT kShowSessionMessage = WM_APP + 2;
std::vector<usb_vault::card::VerifiedLocalArtifact> g_deployment_artifacts;
// The page and its script are verified once and then held open for as long as
// this window exists. The start check closes its own handles when it returns,
// and setup builds no card session, so without these the file that receives
// the whole credential set could be replaced between the check and the
// navigation that loads it.
std::vector<usb_vault::card::VerifiedLocalArtifact> g_ui_artifacts;
usb_vault::ui::CreationWorkState g_creation_work;
// Receives shell drags onto this window. It exists for the whole life of
// the window but stays disarmed until the maintenance page names a
// destination, so every other surface answers a drag with a refusal.
usb_vault::ui::ExternalDropTarget* g_drop_target{};
bool g_deferred_removal = false;
bool g_deferred_arrival = false;

// Presets are client-area dimensions in 96-DPI units. Never place a preset
// outside the current monitor's work area, including its title bar/borders.
bool apply_window_size(const HWND window, const unsigned int preset) noexcept {
  if (window == nullptr || preset > 3) return false;
  if (preset == 2) {
    ::ShowWindow(window, SW_MAXIMIZE);
    return true;
  }
  MONITORINFO monitor{sizeof(monitor)};
  if (!::GetMonitorInfoW(::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         &monitor)) return false;
  const int dpi = static_cast<int>(::GetDpiForWindow(window));
  RECT size{0, 0, ::MulDiv(preset == 3 ? 720 : (preset == 0 ? 1120 : 1400), dpi, 96),
                  ::MulDiv(preset == 3 ? 640 : (preset == 0 ? 760 : 860), dpi, 96)};
  if (!::AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0,
                                 static_cast<UINT>(dpi))) return false;
  const RECT work = monitor.rcWork;
  const int width = (std::min)(size.right - size.left, work.right - work.left);
  const int height = (std::min)(size.bottom - size.top, work.bottom - work.top);
  if (::IsZoomed(window) || ::IsIconic(window)) ::ShowWindow(window, SW_RESTORE);
  return ::SetWindowPos(window, nullptr,
      work.left + (work.right - work.left - width) / 2,
      work.top + (work.bottom - work.top - height) / 2, width, height,
      SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

class MainWindowSizeSink final : public usb_vault::ui::IUiWindowSizeSink {
 public:
  bool set_window_size(const unsigned int preset) noexcept override {
    if (!apply_window_size(g_main_window, preset)) return false;
    try {
      usb_vault::application::set_product_preference_v1(
          usb_vault::application::product_settings_v1_default_path(),
          usb_vault::application::ProductPreference::window_size, preset);
      preset_ = preset;
      return true;
    } catch (...) {
      static_cast<void>(apply_window_size(g_main_window, preset_));
      return false;
    }
  }
  unsigned int window_size() const noexcept override { return preset_; }
  void restore() noexcept {
    try {
      preset_ = usb_vault::application::product_preference_v1(
          usb_vault::application::product_settings_v1_default_path(),
          usb_vault::application::ProductPreference::window_size);
    } catch (...) { preset_ = 1; }
    static_cast<void>(apply_window_size(g_main_window, preset_));
  }
 private:
  unsigned int preset_{1};
};
MainWindowSizeSink g_window_size_sink;

// Posted rather than sent: the reset returns through the WebView2 dispatch
// first, so the window is destroyed after the call that asked for it has
// finished unwinding.
class MainWindowCloseSink final : public usb_vault::ui::IUiCloseRequestSink {
 public:
  bool request_configuration() noexcept override;
  void request_close() noexcept override {
    if (g_main_window != nullptr) {
      static_cast<void>(::PostMessageW(g_main_window, WM_CLOSE, 0, 0));
    }
  }
};

MainWindowCloseSink g_close_sink;

[[nodiscard]] usb_vault::ui::ConfigurationCredentialStatus
stored_credential_status() noexcept {
  using usb_vault::ui::ConfigurationCredentialStatus;
  try {
    static_cast<void>(
        usb_vault::card::vikey::development::WindowsCredentialProvider{}
            .load_credentials());
    return ConfigurationCredentialStatus::ready;
  } catch (const std::system_error& error) {
    // A set that was never written reports the Windows "not found" code.
    // Anything else is a storage or format problem, and saying "missing"
    // there would send the user to retype values that are already stored.
    return usb_vault::card::diagnostic_kind(error) ==
                   static_cast<std::uint32_t>(
                       usb_vault::card::NativeCodeKind::win32) &&
                   usb_vault::card::diagnostic_native(error) == ERROR_NOT_FOUND
               ? ConfigurationCredentialStatus::missing
               : ConfigurationCredentialStatus::unreadable;
  } catch (...) {
    return ConfigurationCredentialStatus::unreadable;
  }
}

// Setup writes the development credential set and hands the process back to
// the product session. It owns no card, volume or container state: the page it
// serves is opened before any session exists, and the session is only built
// after this window has been destroyed.
class ConfigurationHost final : public usb_vault::ui::IConfigurationHost {
 public:
  usb_vault::ui::ConfigurationCredentialStatus credential_status() noexcept
      override {
    return stored_credential_status();
  }

  usb_vault::ui::ConfigurationOutcome save_credentials(
      const std::span<const std::byte> user_sn,
      const std::span<const std::byte> api_key,
      const std::span<const std::byte> user_pin) noexcept override {
    namespace development = usb_vault::card::vikey::development;
    using usb_vault::ui::ConfigurationOutcome;
    using development::CredentialField;
    // The set is validated whole before anything is written, so a stored set
    // is never a mix of accepted and rejected values.
    if (!development::valid_credential_value(CredentialField::user_sn, user_sn) ||
        !development::valid_credential_value(CredentialField::api_key, api_key) ||
        !development::valid_credential_value(CredentialField::user_pin, user_pin)) {
      return ConfigurationOutcome::invalid_format;
    }
    try {
      development::store_credentials({
          .user_sn = locked_copy(user_sn),
          .api_key = locked_copy(api_key),
          .user_pin = locked_copy(user_pin),
      });
      return ConfigurationOutcome::saved;
    } catch (...) {
      // store_credentials reads the entry back; a failure here means the
      // stored set could not be confirmed, which is never a success.
      return ConfigurationOutcome::storage_failure;
    }
  }

  bool continue_to_session() noexcept override {
    // Leaving setup is never refused over the stored set. A session built on
    // a missing or unreadable one cannot load credentials at all, so it fails
    // before the card is contacted and spends no attempt; the way back into
    // setup stays on the login page. Refusing here would instead strand a
    // first run on a page it could not leave.
    if (g_main_window == nullptr) return false;
    if (!::PostMessageW(g_main_window, kShowSessionMessage, 0, 0)) return false;
    g_view_switch_pending = true;
    return true;
  }

 private:
  [[nodiscard]] static usb_vault::security::SecureBuffer locked_copy(
      const std::span<const std::byte> value) {
    usb_vault::security::SecureBuffer result(value.size());
    std::ranges::copy(value, result.bytes().begin());
    return result;
  }
};

ConfigurationHost g_configuration_host;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<IDispatch> g_host_object;
std::wstring g_page_uri;
std::unique_ptr<usb_vault::volume::VolumeProvider> g_volume_provider;
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
std::unique_ptr<
    usb_vault::platform::windows::ShellContainerFileChooser>
    g_container_file_chooser;
std::unique_ptr<usb_vault::ui::CardProductUiSessionController> g_ui_session;
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

void show_failure(const wchar_t* const message) noexcept {
  static_cast<void>(::MessageBoxW(nullptr, message, kWindowTitle,
                                  MB_OK | MB_ICONERROR));
}

void show_numeric_failure(const wchar_t* message,
    const usb_vault::card::FailureDiagnostic failure) noexcept {
  wchar_t text[1536]{};
  const auto& p = failure.primary;
  swprintf_s(text, L"%s\n错误码：%u\n原因码：%u\n底层类别：%u\n底层错误码：%u\n附加清理错误码：%u",
      message, p.code, p.reason, p.native_kind, p.native_code, failure.cleanup.code);
  show_failure(text);
}

void show_numeric_failure(const wchar_t* message, std::uint32_t code,
    std::uint32_t kind = 0, std::uint32_t native = 0) noexcept {
  show_numeric_failure(message, {.primary = {.code = code, .native_kind = kind, .native_code = native}});
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
    usb_vault::card::throw_startup_failure(1002, 1, length ? ERROR_INSUFFICIENT_BUFFER : ::GetLastError());
  }
  if (!::PathRemoveFileSpecW(executable_path.data())) {
    usb_vault::card::throw_startup_failure(1002);
  }
  return executable_path.data();
}

[[nodiscard]] std::wstring child_path(const std::wstring& directory,
                                      const wchar_t* const name) {
  std::vector<wchar_t> page_path(32768);
  if (!::PathCombineW(page_path.data(), directory.c_str(), name)) {
    usb_vault::card::throw_startup_failure(1205);
  }
  return page_path.data();
}

[[nodiscard]] std::wstring local_page_uri() {
  const std::wstring page_path =
      child_path(executable_directory(), L"card_session.html");

  std::vector<wchar_t> uri(32768);
  DWORD uri_size = static_cast<DWORD>(uri.size());
  const HRESULT result =
      ::UrlCreateFromPathW(page_path.c_str(), uri.data(), &uri_size, 0);
  if (FAILED(result)) {
    usb_vault::card::throw_startup_failure(1301, 2, static_cast<std::uint32_t>(result));
  }
  return std::wstring(uri.data(), uri_size);
}

[[nodiscard]] std::array<wchar_t, 2> unused_drive_letters() {
  const DWORD drives = ::GetLogicalDrives();
  if (drives == 0) {
    usb_vault::card::throw_startup_failure(1203, 1, ::GetLastError());
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
    usb_vault::card::throw_startup_failure(1204);
  }
  return result;
}

class LocalArguments final {
 public:
  LocalArguments() {
    values_ = ::CommandLineToArgvW(::GetCommandLineW(), &count_);
    if (values_ == nullptr || count_ <= 0) {
      usb_vault::card::throw_startup_failure(1006, 1, ::GetLastError());
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

// The start check covers the reviewed layout beside this executable. A run
// with explicit development arguments describes its own helper and vendor
// files, so it keeps the behaviour it had before setup moved in here.
[[nodiscard]] bool packaged_deployment() {
  return LocalArguments{}.views().size() == 1;
}

void pin_user_interface(const std::wstring& directory) {
  namespace development = usb_vault::card::vikey::development;
  const auto pin = [&](const wchar_t* const name, const wchar_t* const digest) {
    g_ui_artifacts.push_back(usb_vault::card::verify_local_artifact(
        child_path(directory, name),
        development::parse_deployment_digest(digest), 128ULL * 1024 * 1024));
  };
  try {
    pin(L"card_session.html", p2_deployment::html);
    pin(L"card_session.js", p2_deployment::js);
  } catch (...) {
    g_ui_artifacts.clear();
    throw usb_vault::card::LocalFailureError(usb_vault::card::capture_startup_failure(1207));
  }
}

void initialize_application_session([[maybe_unused]] const HWND window) {
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
  const LocalArguments arguments;
  const std::vector<std::wstring_view> views = arguments.views();
  namespace development = usb_vault::card::vikey::development;
  auto config = [&] {
    try {
      return views.size() == 1
          ? development::packaged_app_config(directory, p2_deployment::helper, p2_deployment::vendor)
          : development::parse_app_config(views);
    } catch (...) {
      throw usb_vault::card::LocalFailureError(
          usb_vault::card::capture_startup_failure(views.size() == 1 ? 1205 : 1006));
    }
  }();
  if (views.size() == 1) {
    try {
      const auto pin = [&](const wchar_t* name, const wchar_t* digest) {
        g_deployment_artifacts.push_back(usb_vault::card::verify_local_artifact(
            child_path(directory, name), development::parse_deployment_digest(digest),
            128ULL * 1024 * 1024));
      };
      pin(L"usb_vault_vikey_helper.exe", p2_deployment::helper);
      pin(L"usb_vault_volume_creator.exe", p2_deployment::creator);
      pin(L"VeraCryptFormat.dll", p2_deployment::format);
      pin(L"vendor\\ViKey.dll", p2_deployment::vendor);
      pin(L"card_session.html", p2_deployment::html);
      pin(L"card_session.js", p2_deployment::js);
    } catch (...) {
      g_deployment_artifacts.clear();
      throw usb_vault::card::LocalFailureError(usb_vault::card::capture_startup_failure(1207));
    }
  }
  auto credential_provider = std::make_unique<
      usb_vault::card::vikey::development::WindowsCredentialProvider>();
  auto enrollment = std::make_unique<
      usb_vault::card::vikey::VikeyHelperCardEnrollment>(
          std::move(config),
          *credential_provider);
  auto entry = std::make_unique<
      usb_vault::application::CardEntryContextController>(*enrollment);
  auto authentication = std::make_unique<
      usb_vault::application::CardAuthenticationSession>();
  auto volume_session = std::make_unique<
      usb_vault::application::CardLoginMountSession>(
          *authentication, *g_volume_provider, std::move(layout),
          child_path(directory, L"usb_vault_volume_creator.exe"),
          usb_vault::application::LoginMountPolicy::manual);
  auto password_policy = std::make_unique<
      usb_vault::security::OfflineCardPasswordPolicy>();
  auto file_chooser = std::make_unique<
      usb_vault::platform::windows::ShellContainerFileChooser>(window);
  auto ui_session = std::make_unique<
      usb_vault::ui::CardProductUiSessionController>(
          *entry, *authentication, *volume_session, *password_policy,
          *file_chooser);
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
  g_container_file_chooser = std::move(file_chooser);
  g_ui_session = std::move(ui_session);
  g_card_presence_controller = std::move(presence);
  g_card_lifecycle_controller = std::move(lifecycle);
  g_card_device_notifications = std::move(notifications);
}

// Looked up when the copy starts rather than captured: the product session
// is torn down and rebuilt whenever the surface changes, and a drop that
// outlived one would otherwise be writing through a released controller.
void import_dropped_entry(const std::uint32_t id,
                          const std::wstring_view relative_directory,
                          const std::wstring_view absolute_source,
                          const std::function<bool()>& cancelled) {
  if (g_ui_session == nullptr) {
    throw std::runtime_error("no product session is open for a shell drop");
  }
  g_ui_session->import_external_entry(id, relative_directory, absolute_source,
                                      cancelled);
}

void disarm_external_drops() noexcept {
  if (g_drop_target != nullptr) g_drop_target->clear_target();
}

void clear_application_session() noexcept {
  disarm_external_drops();
  g_card_device_notifications.reset();
  g_card_lifecycle_controller.reset();
  g_card_presence_controller.reset();
  g_ui_session.reset();
  g_container_file_chooser.reset();
  g_password_policy.reset();
  g_card_volume_session.reset();
  g_authentication.reset();
  g_card_entry.reset();
  g_card_enrollment.reset();
  g_volume_provider.reset();
  g_vikey_credential_provider.reset();
  g_deployment_artifacts.clear();
}

bool MainWindowCloseSink::request_configuration() noexcept {
  if (!g_main_window || !g_ui_session || g_creation_work.running.load() ||
      g_ui_session->state() == usb_vault::ui::CardProductUiState::authenticated ||
      g_ui_session->mounted()) return false;
  if (!::PostMessageW(g_main_window, kShowConfigurationMessage, 0, 0)) {
    return false;
  }
  g_view_switch_pending = true;
  return true;
}

void report_webview_failure(std::uint32_t code, HRESULT result = S_OK,
    std::uint32_t event_kind = 0, std::uint32_t event_code = 0,
    std::uint32_t step = 0) noexcept;

void resize_webview(const HWND window) noexcept {
  if (!g_controller) {
    return;
  }
  RECT bounds{};
  if (!::GetClientRect(window, &bounds)) {
    report_webview_failure(1316, HRESULT_FROM_WIN32(::GetLastError()), 0, 0, 1);
    return;
  }
  const HRESULT result = g_controller->put_Bounds(bounds);
  if (FAILED(result)) report_webview_failure(1316, result, 0, 0, 2);
}

void notify_webview_session_changed() noexcept {
  if (!g_webview || !g_webview_startup.current(g_page_generation)) return;
  // Only the bounded numeric failure snapshot crosses this boundary. Never
  // insert an exception message, SDK payload, file path, or card identity.
  const auto failure = g_card_entry ? g_card_entry->startup_failure()
                                    : usb_vault::card::FailureDiagnostic{};
  const auto& primary = failure.primary;
  std::wstring script;
  try {
    script = L"window.usbVaultStartupFailure={code:" + std::to_wstring(primary.code) +
        L",reason:" + std::to_wstring(primary.reason) +
        L",nativeKind:" + std::to_wstring(primary.native_kind) +
        L",nativeCode:" + std::to_wstring(primary.native_code) +
        L",cleanupCode:" + std::to_wstring(failure.cleanup.code) + L"};" +
        usb_vault::ui::kWebviewSessionChangedScript;
  } catch (...) {
    show_failure(L"无法准备界面诊断信息。错误码：9002。请重新打开应用。");
    return;
  }
  const auto page_generation = g_page_generation;
  const HRESULT result = g_webview->ExecuteScript(
      script.c_str(),
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [page_generation](const HRESULT script_result, LPCWSTR) -> HRESULT {
            if (!g_webview_startup.current(page_generation)) return S_OK;
            if (FAILED(script_result)) {
              show_failure(
                  L"安全会话状态已变化，但界面刷新失败。错误码：1504。请重新打开应用确认状态。");
            }
            return S_OK;
          })
          .Get());
  if (FAILED(result)) {
    show_failure(
        L"安全会话状态已变化，但界面刷新失败。错误码：1504。请重新打开应用确认状态。");
  }
}

// Installs the bridge the current surface is entitled to. Setup gets one of
// its own: the product surface is not merely hidden there, it is absent, so a
// page shown before any credential exists cannot reach the card, a container
// or a volume.
[[nodiscard]] HRESULT attach_host_object() noexcept {
  if (!g_webview) return E_FAIL;
  ComPtr<IDispatch> host_object;
  HRESULT result = g_view == StartupView::configuration
      ? usb_vault::ui::create_card_configuration_webview_host_object(
            g_configuration_host, &host_object)
      : usb_vault::ui::create_card_product_webview_host_object(
            *g_ui_session, &g_close_sink, &host_object, &g_window_size_sink,
            &g_creation_work, g_drop_target);
  if (FAILED(result)) return result;
  VARIANT host_value{};
  ::VariantInit(&host_value);
  host_value.vt = VT_DISPATCH;
  host_value.pdispVal = host_object.Get();
  host_value.pdispVal->AddRef();
  result = g_webview->AddHostObjectToScript(kHostObjectName, &host_value);
  static_cast<void>(::VariantClear(&host_value));
  if (FAILED(result)) return result;
  g_host_object = std::move(host_object);
  return S_OK;
}

// Retires the running surface: the page can no longer call anything, the
// bridge this process holds is dropped, and the document that owns the last
// reference to it is replaced by the caller's navigation.
// Watched from the existing card timer rather than pushed from the worker:
// the notice has to reach the page on the UI thread anyway, and the timer is
// already there. It repeats once a second while a drop runs so a notice the
// page missed cannot leave it unaware that one is going.
void notify_webview_external_drop(const unsigned int state) noexcept {
  static unsigned int last_state = 0;
  static unsigned int ticks = 0;
  const bool repeat = state == 1 && ++ticks >= 10;
  if (state == last_state && !repeat) return;
  last_state = state;
  ticks = 0;
  if (!g_webview) return;
  static_cast<void>(g_webview->ExecuteScript(
      usb_vault::ui::kWebviewExternalDropChangedScript,
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; })
          .Get()));
}

void detach_host_object() noexcept {
  if (g_webview) {
    static_cast<void>(g_webview->RemoveHostObjectFromScript(kHostObjectName));
  }
  g_host_object.Reset();
}

void report_webview_failure(const std::uint32_t code,
                            const HRESULT result, const std::uint32_t event_kind,
                            const std::uint32_t event_code, const std::uint32_t step) noexcept {
  if (!g_webview_startup.fail(g_page_generation)) return;
  ::KillTimer(g_main_window, 2);
  disarm_external_drops();
  detach_host_object();
  const wchar_t* reason = L"界面无法完成初始化。";
  switch (code) {
    case 1302: case 1303: reason = L"无法创建 WebView2 运行环境。"; break;
    case 1304: case 1305: case 1306: reason = L"无法创建网页显示窗口。"; break;
    case 1307: reason = L"WebView2 缺少所需接口或接口不可访问。"; break;
    case 1308: reason = L"无法启用网页安全设置，已停止加载。"; break;
    case 1309: reason = L"无法连接界面与本地程序。"; break;
    case 1310: case 1311: reason = L"无法建立或执行网页导航保护。"; break;
    case 1312: case 1313: reason = L"无法加载本地界面。"; break;
    case 1314: reason = L"网页进程异常或无法建立进程监测。"; break;
    case 1315: reason = L"等待界面就绪超时。"; break;
    case 1316: reason = L"无法设置网页窗口显示区域。"; break;
    case 1408: reason = L"页面启动脚本发生异常。"; break;
    case 1401: case 1402: case 1403: case 1404: case 1405:
      reason = L"界面与本地程序的启动通信失败。"; break;
  }
  wchar_t message[768]{};
  swprintf_s(message, L"%s\n错误码：%u\n系统错误类别：%u\n系统错误码：%u\n事件类别：%u\n事件码：%u\n步骤码：%u\n请重新打开 USB Vault；仍失败请提供这些编号。",
      reason, code, FAILED(result) ? 2u : 0u, FAILED(result) ? static_cast<unsigned int>(result) : 0u,
      event_kind, event_code, step);
  show_failure(message);
  // Use the normal close guard. Never destroy an authenticated session with
  // active volumes merely because its renderer failed.
  if (::IsWindow(g_main_window)) ::PostMessageW(g_main_window, WM_CLOSE, 0, 0);
}

void begin_webview_watch(const HWND window) {
  ++g_page_generation;
  g_navigation_id = 0;
  g_navigation_complete = false;
  g_ready_poll_pending = false;
  g_webview_startup.begin(g_page_generation, ::GetTickCount64());
  if (::SetTimer(window, 2, 250, nullptr) == 0) {
    report_webview_failure(1209, HRESULT_FROM_WIN32(::GetLastError()));
  }
}

void poll_webview_ready() {
  if (!g_webview_startup.current(g_page_generation)) return;
  if (g_webview_startup.expired(g_page_generation, ::GetTickCount64())) {
    report_webview_failure(1315);
    return;
  }
  if (!g_webview || !g_navigation_complete || g_ready_poll_pending) return;
  g_ready_poll_pending = true;
  const auto generation = g_page_generation;
  const HRESULT result = g_webview->ExecuteScript(
      L"Number(window.usbVaultPageStartupStatus || 0)",
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [generation](HRESULT status, LPCWSTR value) -> HRESULT {
            if (!g_webview_startup.current(generation)) return S_OK;
            g_ready_poll_pending = false;
            if (FAILED(status)) { report_webview_failure(1401, status); return S_OK; }
            const std::wstring_view text = value ? value : L"";
            if (text == L"1") {
              static_cast<void>(g_webview_startup.ready(generation));
              ::KillTimer(g_main_window, 2);
              notify_webview_session_changed();
            } else if (const auto error = usb_vault::ui::page_startup_error(text)) {
              report_webview_failure(error);
            }
            return S_OK;
          }).Get());
  if (FAILED(result)) {
    g_ready_poll_pending = false;
    report_webview_failure(1401, result);
  }
}

void initialize_webview(const HWND window) {
  const auto generation = ++g_window_generation;
  begin_webview_watch(window);
  if (!g_webview_startup.current(g_page_generation)) return;
  try { g_page_uri = local_page_uri(); }
  catch (...) {
    const auto failure = usb_vault::card::capture_startup_failure(1301).primary;
    report_webview_failure(failure.code, failure.native_kind == 2 ? static_cast<HRESULT>(failure.native_code) : S_OK);
    return;
  }
  const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [window, generation](const HRESULT environment_result,
                   ICoreWebView2Environment* const environment) -> HRESULT {
            if (generation != g_window_generation || !::IsWindow(window) ||
                !g_webview_startup.current(g_page_generation)) return S_OK;
            if (FAILED(environment_result) || environment == nullptr) {
              report_webview_failure(1303, FAILED(environment_result) ? environment_result : E_POINTER);
              return FAILED(environment_result) ? environment_result : E_FAIL;
            }
            const HRESULT create_result = environment->CreateCoreWebView2Controller(
                window,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [window, generation](const HRESULT controller_result,
                             ICoreWebView2Controller* const controller) -> HRESULT {
                      if (generation != g_window_generation || !::IsWindow(window) ||
                          !g_webview_startup.current(g_page_generation)) {
                        if (controller) controller->Close();
                        return S_OK;
                      }
                      if (FAILED(controller_result) || controller == nullptr) {
                        report_webview_failure(1305, FAILED(controller_result) ? controller_result : E_POINTER);
                        return FAILED(controller_result) ? controller_result
                                                         : E_FAIL;
                      }
                      g_controller = controller;
                      HRESULT current = g_controller->get_CoreWebView2(&g_webview);
                      if (FAILED(current) || !g_webview) {
                        report_webview_failure(1306, FAILED(current) ? current : E_POINTER);
                        return FAILED(current) ? current : E_FAIL;
                      }

                      ComPtr<ICoreWebView2Settings> settings;
                      current = g_webview->get_Settings(&settings);
                      if (FAILED(current)) {
                        report_webview_failure(1307, current);
                        return current;
                      }
                      current = settings->put_AreDevToolsEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 1);
                        return current;
                      }
                      current =
                          settings->put_AreDefaultContextMenusEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 2);
                        return current;
                      }
                      current =
                          settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 3);
                        return current;
                      }
                      current = settings->put_IsWebMessageEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 4);
                        return current;
                      }
                      current = settings->put_IsStatusBarEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 5);
                        return current;
                      }

                      ComPtr<ICoreWebView2Settings4> restricted_settings;
                      current = settings.As(&restricted_settings);
                      if (FAILED(current)) {
                        report_webview_failure(1307, current);
                        return current;
                      }
                      current = restricted_settings
                                    ->put_AreBrowserAcceleratorKeysEnabled(
                                        FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 6);
                        return current;
                      }
                      current = restricted_settings
                                    ->put_IsPasswordAutosaveEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 7);
                        return current;
                      }
                      current = restricted_settings
                                    ->put_IsGeneralAutofillEnabled(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 8);
                        return current;
                      }

                      // 🔴 The page must never receive a shell drop. What a
                      // dragged file is, and whether it may be written into a
                      // volume, is decided in the native window, which is the
                      // only place that has both the file and the session;
                      // handing the document the drop would put a path from
                      // outside onto the page boundary that exists to keep
                      // paths off it. Refusing here is also what lets the drag
                      // fall through to this window's own drop target.
                      ComPtr<ICoreWebView2Controller4> drop_controller;
                      current = g_controller.As(&drop_controller);
                      if (FAILED(current)) {
                        report_webview_failure(1307, current);
                        return current;
                      }
                      current =
                          drop_controller->put_AllowExternalDrop(FALSE);
                      if (FAILED(current)) {
                        report_webview_failure(1308, current, 0, 0, 9);
                        return current;
                      }

                      current = attach_host_object();
                      if (FAILED(current)) {
                        report_webview_failure(1309, current);
                        return current;
                      }

                      EventRegistrationToken navigation_token{};
                      current = g_webview->add_NavigationStarting(
                          Callback<ICoreWebView2NavigationStartingEventHandler>(
                              [generation](ICoreWebView2*,
                                 ICoreWebView2NavigationStartingEventArgs* args)
                                  -> HRESULT {
                                if (generation != g_window_generation ||
                                    !g_webview_startup.current(g_page_generation)) {
                                  return args->put_Cancel(TRUE);
                                }
                                LPWSTR raw_uri = nullptr;
                                const HRESULT uri_result = args->get_Uri(&raw_uri);
                                if (FAILED(uri_result)) {
                                  static_cast<void>(args->put_Cancel(TRUE));
                                  report_webview_failure(1311, uri_result);
                                  return uri_result;
                                }
                                const bool allowed = raw_uri && std::wstring_view(raw_uri) == g_page_uri;
                                ::CoTaskMemFree(raw_uri);
                                if (!allowed) {
                                  const HRESULT canceled = args->put_Cancel(TRUE);
                                  if (FAILED(canceled)) report_webview_failure(1311, canceled);
                                } else {
                                  const HRESULT identified = args->get_NavigationId(&g_navigation_id);
                                  if (FAILED(identified)) report_webview_failure(1311, identified);
                                }
                                return S_OK;
                              })
                              .Get(),
                          &navigation_token);
                      if (FAILED(current)) {
                        report_webview_failure(1310, current);
                        return current;
                      }
                      EventRegistrationToken completed_token{};
                      current = g_webview->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [generation](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                if (generation != g_window_generation ||
                                    !g_webview_startup.current(g_page_generation)) return S_OK;
                                UINT64 navigation{};
                                const HRESULT id_result = args->get_NavigationId(&navigation);
                                if (FAILED(id_result)) { report_webview_failure(1313, id_result); return S_OK; }
                                if (navigation != g_navigation_id) return S_OK;
                                BOOL success = FALSE;
                                const HRESULT status = args->get_IsSuccess(&success);
                                if (FAILED(status)) {
                                  report_webview_failure(1313, status, 0, 0, 1);
                                  return S_OK;
                                }
                                if (!success) {
                                  COREWEBVIEW2_WEB_ERROR_STATUS error{};
                                  const HRESULT detail = args->get_WebErrorStatus(&error);
                                  report_webview_failure(1313, detail, SUCCEEDED(detail) ? 1 : 0,
                                      SUCCEEDED(detail) ? static_cast<std::uint32_t>(error) : 0, 2);
                                  return S_OK;
                                }
                                g_navigation_complete = true;
                                return S_OK;
                              }).Get(), &completed_token);
                      if (FAILED(current)) { report_webview_failure(1310, current); return current; }
                      EventRegistrationToken failed_token{};
                      current = g_webview->add_ProcessFailed(
                          Callback<ICoreWebView2ProcessFailedEventHandler>(
                              [generation](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                if (generation != g_window_generation || !g_webview_startup.current(g_page_generation)) return S_OK;
                                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                const HRESULT result = args ? args->get_ProcessFailedKind(&kind) : E_POINTER;
                                report_webview_failure(1314, result, SUCCEEDED(result) ? 2 : 0,
                                    SUCCEEDED(result) ? static_cast<std::uint32_t>(kind) : 0);
                                return S_OK;
                              }).Get(), &failed_token);
                      if (FAILED(current)) { report_webview_failure(1314, current); return current; }
                      resize_webview(window);
                      if (!g_webview_startup.current(g_page_generation)) return E_FAIL;
                      current = g_webview->Navigate(g_page_uri.c_str());
                      if (FAILED(current)) report_webview_failure(1312, current);
                      return current;
                    })
                    .Get());
            if (FAILED(create_result)) report_webview_failure(1304, create_result);
            return create_result;
          })
          .Get());
  if (FAILED(result)) {
    report_webview_failure(1302, result);
  }
}

// The window, its WebView2 controller and its size stay exactly as they are.
// Only the card session, the bridge and the document change, so setup is a
// page of this application rather than a second window that replaces it.
void show_configuration(const HWND window) noexcept {
  g_view_switch_pending = false;
  begin_webview_watch(window);
  if (!g_webview_startup.current(g_page_generation)) return;
  static_cast<void>(::KillTimer(window, 1));
  disarm_external_drops();
  detach_host_object();
  // The retiring product bridge may outlive this call inside the old
  // document, but it stopped serving when the swap was accepted, so it can no
  // longer reach the controller released here.
  clear_application_session();
  g_deferred_removal = false;
  g_deferred_arrival = false;
  g_view = StartupView::configuration;
  const HRESULT attached = attach_host_object();
  if (FAILED(attached)) { report_webview_failure(1309, attached); return; }
  const HRESULT navigated = g_webview->Navigate(g_page_uri.c_str());
  if (FAILED(navigated)) report_webview_failure(1312, navigated);
}

void show_session(const HWND window) noexcept {
  g_view_switch_pending = false;
  g_webview_startup.stop();
  ::KillTimer(window, 2);
  disarm_external_drops();
  detach_host_object();
  g_view = StartupView::session;
  try {
    // The card is probed again from here, so the session that starts is built
    // on the credentials that were just saved rather than the ones it began
    // with.
    initialize_application_session(window);
  } catch (...) {
    show_numeric_failure(L"无法重新建立安全会话。请重新启动 USB Vault。",
        usb_vault::card::capture_startup_failure(1210));
    static_cast<void>(::DestroyWindow(window));
    return;
  }
  begin_webview_watch(window);
  if (!g_webview_startup.current(g_page_generation)) return;
  if (::SetTimer(window, 1, 100, nullptr) == 0) {
    report_webview_failure(1209, HRESULT_FROM_WIN32(::GetLastError())); return;
  }
  const HRESULT attached = attach_host_object();
  if (FAILED(attached)) { report_webview_failure(1309, attached); return; }
  const HRESULT navigated = g_webview->Navigate(g_page_uri.c_str());
  if (FAILED(navigated)) report_webview_failure(1312, navigated);
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) try {
  switch (message) {
    case kShowConfigurationMessage:
      show_configuration(window);
      return 0;
    case kShowSessionMessage:
      show_session(window);
      return 0;
    case WM_DEVICECHANGE:
      if (g_view_switch_pending) return 0;
      if (g_creation_work.running.load() || g_deferred_removal || g_deferred_arrival) {
        const auto arrival = usb_vault::platform::windows::inspect_target_card_event(
            wparam, lparam, kDevelopmentCardIdentity);
        if (arrival.has_value()) {
          g_creation_work.card_epoch.fetch_add(1);
          disarm_external_drops();
          if (!*arrival) { g_deferred_removal = true; g_creation_work.interrupted.store(true); }
          g_deferred_arrival = *arrival;
        }
        return 0;
      }
      if (g_card_device_notifications != nullptr &&
          g_card_lifecycle_controller != nullptr) {
        const auto transition =
            usb_vault::platform::windows::handle_hid_device_change(
                *g_card_lifecycle_controller, wparam, lparam,
                kDevelopmentCardIdentity);
        using Transition = usb_vault::application::CardPresenceTransition;
        if (transition.entry_changed ||
            transition.presence != Transition::no_change) {
          g_creation_work.card_epoch.fetch_add(1);
          // The page re-arms from the maintenance screen it rebuilds; until
          // then a drag onto this window has nowhere it is allowed to land.
          disarm_external_drops();
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
      }
      return 0;
    case WM_TIMER:
      if (g_view_switch_pending) return 0;
      if (wparam == 2) { poll_webview_ready(); return 0; }
      if (wparam == 1 && g_drop_target != nullptr) {
        notify_webview_external_drop(g_drop_target->status().state);
      }
      if (wparam == 1 && !g_creation_work.running.load() && g_card_lifecycle_controller) {
        const bool changed = g_deferred_removal || g_deferred_arrival;
        if (g_deferred_removal) {
          const auto transition = g_card_lifecycle_controller->handle_removal();
          using Transition = usb_vault::application::CardPresenceTransition;
          if (transition.presence == Transition::active_session_force_unmounted) {
            show_warning(L"安全卡已拔出，磁盘正在被占用，已强制卸载。未写入磁盘的数据可能已丢失。");
          }
          if (transition.presence == Transition::active_session_remains) {
            show_failure(L"安全卡已拔出，强制卸载仍然失败。请关闭占用磁盘的程序并重试安全卸载。");
          }
          g_deferred_removal = false;
        }
        if (g_deferred_arrival) {
          static_cast<void>(g_card_lifecycle_controller->handle_arrival());
          g_deferred_arrival = false;
        }
        g_creation_work.interrupted.store(false);
        if (changed) notify_webview_session_changed();
      }
      return 0;
    case WM_SIZE:
      resize_webview(window);
      return 0;
    // Moved to a display scaled differently. Windows hands over the rectangle
    // the window should take at the new scale; taking it keeps the window the
    // same physical size across the move, and WM_SIZE then re-lays the WebView
    // out. WebView2 rescales its own content from the window's DPI.
    case WM_DPICHANGED: {
      const RECT* const suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        static_cast<void>(::SetWindowPos(
            window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE));
      }
      return 0;
    }
    case WM_CLOSE:
      if (g_creation_work.running.load()) {
        if (g_creation_work.volume_operation.load()) {
          ::MessageBoxW(window, L"正在处理磁盘操作，请等待处理结果后再关闭。",
                        kWindowTitle, MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        if (!g_creation_work.can_cancel()) {
          ::MessageBoxW(window, g_creation_work.cancellation_requested()
              ? L"正在取消创建，请等待处理结果后再关闭。"
              : L"正在完成创建，已无法取消。请等待处理结果后再关闭。",
              kWindowTitle, MB_OK | MB_ICONINFORMATION);
          return 0;
        }
        if (::MessageBoxW(window,
                          L"容器正在创建。要取消创建吗？"
                          L"应用会等待处理结束，请查看处理结果后再关闭。",
                          kWindowTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) {
          if (!g_creation_work.request_cancel()) {
            ::MessageBoxW(window, L"取消未生效，创建已进入完成阶段或已结束。请查看处理结果后再关闭。",
                          kWindowTitle, MB_OK | MB_ICONINFORMATION);
          }
        }
        return 0;
      }
      if (g_ui_session != nullptr &&
          !usb_vault::ui::prepare_ui_session_for_close(*g_ui_session)) {
        show_failure(L"无法安全卸载当前磁盘，请关闭占用文件后重试。");
        return 0;
      }
      static_cast<void>(::DestroyWindow(window));
      return 0;
    case WM_DESTROY:
      ++g_window_generation;
      g_webview_startup.stop();
      ::KillTimer(window, 1);
      ::KillTimer(window, 2);
      // Before the session it is writing into goes: revoking stops new drags,
      // and closing waits for the copy that is already running.
      if (g_drop_target != nullptr) {
        static_cast<void>(::RevokeDragDrop(window));
        g_drop_target->close();
        static_cast<void>(g_drop_target->Release());
        g_drop_target = nullptr;
      }
      if (g_webview) g_webview->RemoveHostObjectFromScript(kHostObjectName);
      if (g_controller) g_controller->Close();
      g_card_device_notifications.reset();
      g_webview.Reset();
      g_controller.Reset();
      g_host_object.Reset();
      ::PostQuitMessage(0);
      return 0;
    default:
      return ::DefWindowProcW(window, message, wparam, lparam);
  }
} catch (...) {
  const auto failure = usb_vault::card::capture_startup_failure(9001);
  if (g_webview_startup.current(g_page_generation)) {
    const auto& primary = failure.primary;
    report_webview_failure(primary.code, primary.native_kind == 2 ? static_cast<HRESULT>(primary.native_code) :
        primary.native_kind == 1 ? HRESULT_FROM_WIN32(primary.native_code) : S_OK);
  } else if (g_main_window == nullptr) {
    show_numeric_failure(L"窗口初始化回调发生异常。", failure);
  }
  // Never allow a C++ exception to cross the Windows callback boundary.
  return 0;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE,
                    _In_ LPWSTR, _In_ int show_command) try {
  try {
    if (!usb_vault::platform::windows::installation_ready_for_start(executable_directory())) {
      show_numeric_failure(L"USB Vault 正在安装／卸载，或上次更新尚未完成。请等待操作结束；如已中断，请重新运行同一个安装包恢复。请勿删除更新备份文件夹。", 1003);
      return 1;
    }
  } catch (...) {
    show_numeric_failure(L"无法确认 USB Vault 的安装状态，请检查安装位置和访问权限。",
        usb_vault::card::capture_startup_failure(1004));
    return 1;
  }
  // OLE rather than COM alone: the window registers a drop target, and
  // `RegisterDragDrop` is refused on an apartment that only initialized COM.
  const HRESULT com_result = ::OleInitialize(nullptr);
  if (FAILED(com_result)) {
    show_numeric_failure(L"COM 初始化失败。", 1005, 2, static_cast<std::uint32_t>(com_result));
    return 1;
  }
  struct OleScope final { ~OleScope() noexcept { ::OleUninitialize(); } } ole_scope;

  // This preflight check never touches the card. The later session probe can
  // authenticate an inserted card. Missing dependencies
  // belong to the installer and end the start here; missing credentials open
  // the product setup page instead of the session.
  if (packaged_deployment()) {
    const auto startup = configuration::inspect_startup();
    if (!startup.dependencies_ready) {
      show_numeric_failure(startup.detail.c_str(), startup.failure);
      return 1;
    }
    if (!startup.credentials_ready) {
      g_view = StartupView::configuration;
      if (startup.credential_failure.primary.code != 1107)
        show_numeric_failure(L"已保存凭据无法读取，将打开配置页；请先确认原因再保存。", startup.credential_failure);
    }
  }

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&window_class) == 0) {
    show_numeric_failure(L"窗口类注册失败。", 1201, 1, ::GetLastError());
    return 1;
  }

  // The manifest makes this process per-monitor DPI aware, so these are real
  // pixels rather than 96-DPI ones Windows would afterwards stretch. Scaling
  // them keeps the window the size it used to look, instead of shrinking it by
  // whatever the display is scaled to.
  const UINT startup_dpi = ::GetDpiForSystem();
  const HWND window = ::CreateWindowExW(
      0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, ::MulDiv(900, static_cast<int>(startup_dpi), 96),
      ::MulDiv(640, static_cast<int>(startup_dpi), 96), nullptr, nullptr,
      instance, nullptr);
  g_main_window = window;
  if (window == nullptr) {
    show_numeric_failure(L"窗口创建失败。", 1202, 1, ::GetLastError());
    return 1;
  }

  g_window_size_sink.restore();

  // Registered before the page exists and disarmed until the maintenance
  // screen names a destination, so the window answers every other drag with a
  // refusal rather than letting one reach the document.
  g_drop_target = new (std::nothrow) usb_vault::ui::ExternalDropTarget(
      import_dropped_entry, g_creation_work);
  const HRESULT drop_result = g_drop_target ? ::RegisterDragDrop(window, g_drop_target) : E_OUTOFMEMORY;
  if (FAILED(drop_result)) {
    if (g_drop_target != nullptr) {
      static_cast<void>(g_drop_target->Release());
      g_drop_target = nullptr;
    }
    show_numeric_failure(L"无法接管窗口的拖放，拖入文件将不可用。", 1208, 2, static_cast<std::uint32_t>(drop_result));
  }

  try {
    if (packaged_deployment()) pin_user_interface(executable_directory());
    // Setup builds no card context and registers no device notification, so
    // an inserted card is never probed with credentials the user is still
    // entering and no PIN attempt can be spent here.
    if (g_view == StartupView::session) {
      initialize_application_session(window);
    }
    initialize_webview(window);
    if (g_view == StartupView::session &&
        ::SetTimer(window, 1, 100, nullptr) == 0)
      usb_vault::card::throw_startup_failure(1209, 1, ::GetLastError());
  } catch (...) {
    show_numeric_failure(L"本地界面或安全会话初始化失败。",
        usb_vault::card::capture_startup_failure(1206));
    ::DestroyWindow(window);
    clear_application_session();
    return 1;
  }

  ::ShowWindow(window,
               g_window_size_sink.window_size() == 2 ? SW_MAXIMIZE
                                                     : show_command);
  static_cast<void>(::UpdateWindow(window));
  MSG message{};
  BOOL message_result{};
  while ((message_result = ::GetMessageW(&message, nullptr, 0, 0)) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
  if (message_result == -1) {
    show_numeric_failure(L"窗口消息循环失败。", 1211, 1, ::GetLastError());
  }

  clear_application_session();
  g_ui_artifacts.clear();
  g_main_window = nullptr;
  return message_result == -1 ? 1 : 0;
} catch (...) {
  show_numeric_failure(L"应用启动发生未处理的异常。",
      usb_vault::card::capture_startup_failure(9001));
  return 1;
}
