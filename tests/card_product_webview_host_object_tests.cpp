#include "usb_vault/ui/card_product_webview_host_object.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/offline_card_password_policy.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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
using usb_vault::ui::create_card_product_webview_host_object;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] SecureBuffer root_secret() {
  SecureBuffer result(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

[[nodiscard]] bool equals(const std::span<const std::byte> left,
                          const std::span<const std::byte> right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

class FakeVolumeSession final : public IAuthenticatedCardVolumeSession {
 public:
  explicit FakeVolumeSession(CardAuthenticationSession& authentication)
      : authentication_(authentication) {}

  void create_container_a(const std::uint64_t size_bytes) override {
    ++create_a_calls;
    created_a_size_bytes = size_bytes;
    if (reject_create_a) throw std::runtime_error("creation failed");
    container_a_exists = true;
    container_a_size_bytes = size_bytes;
    container_a_mounted = true;
    mounted_state = true;
  }

  std::uint32_t create_container(const std::wstring_view path,
                                 const std::uint64_t size_bytes) override {
    ++create_calls;
    created_size_bytes = size_bytes;
    if (reject_create) throw std::runtime_error("creation failed");
    return adopt(path, size_bytes);
  }

  std::uint32_t create_container_b(
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer&) override {
    ++create_b_calls;
    created_outer_bytes = outer_size_bytes;
    created_hidden_bytes = hidden_size_bytes;
    return adopt(L"C:/vault/b.hc", outer_size_bytes);
  }

  SecureBuffer copy_session_password() override {
    const std::vector<std::byte> retained = bytes("login-password");
    SecureBuffer copy(retained.size());
    for (std::size_t index = 0; index < retained.size(); ++index) {
      copy.bytes()[index] = retained[index];
    }
    return copy;
  }

  std::uint64_t created_outer_bytes{};
  std::uint64_t created_hidden_bytes{};
  int create_b_calls{};

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

  void mount_adopted_container(const std::uint32_t id) override {
    ++mount_adopted_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = true;
      mounted_state = true;
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  int mount_adopted_calls{};
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
    if (fail_container_statuses) {
      throw std::runtime_error("container status query failed");
    }
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
    const std::size_t separator = path.find_last_of(L"\\/");
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
  bool reject_create{};
  bool fail_container_statuses{};
  bool fail_operation_history{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};

  [[nodiscard]] ContainerAStatus container_a_status() override {
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_size_bytes,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  [[nodiscard]] std::vector<usb_vault::application::OperationEvent>
  operation_history() override {
    if (fail_operation_history) {
      throw std::runtime_error("operation history query failed");
    }
    return operation_events;
  }

  void release_session_secrets() noexcept override {}

  void clear_operation_history() noexcept override {
    ++clear_operation_history_calls;
    operation_events.clear();
  }

  void consume_authenticated_login(SecureBuffer root,
                                   SecureBuffer login_password_utf8) override {
    ++consume_calls;
    login_bytes.assign(login_password_utf8.bytes().begin(),
                       login_password_utf8.bytes().end());
    authentication_.establish(std::move(root));
    login_password_utf8.clear();
    mounted_state = mount_on_login;
  }

  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) throw std::runtime_error("unmount failed");
    container_a_mounted = false;
    mounted_state = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("forced unmount is outside the UI contract");
  }

  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  CardAuthenticationSession& authentication_;
  std::vector<std::byte> login_bytes;
  std::vector<usb_vault::application::OperationEvent> operation_events;
  bool mount_on_login{};
  bool mounted_state{};
  bool reject_unmount{};
  bool reject_create_a{};
  bool container_a_exists{};
  bool container_a_mounted{};
  std::uint64_t container_a_size_bytes{};
  std::uint64_t created_a_size_bytes{};
  int consume_calls{};
  int create_a_calls{};
  int unmount_calls{};
  int clear_operation_history_calls{};
};

class FakeEntry final : public ICardEntryActions {
 public:
  void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      ConfirmedPassword confirmed_daily_password) override {
    ++initialization_calls;
    SecureBuffer password = std::move(confirmed_daily_password).take();
    initialization_bytes.assign(password.bytes().begin(),
                                password.bytes().end());
    password.clear();
    route = CardEntryRoute::unavailable;
    authentication.establish(root_secret());
  }

  void authenticate_login(
      usb_vault::application::ICardLoginSuccessConsumer& success_consumer,
      SecureBuffer login_password_utf8) override {
    ++login_calls;
    if (reject_credential) {
      throw usb_vault::card::CardCredentialRejected();
    }
    route = CardEntryRoute::unavailable;
    success_consumer.consume_authenticated_login(
        root_secret(), std::move(login_password_utf8));
  }

  void set_second_card_password(SecureBuffer existing_password_utf8,
                                SecureBuffer second_password_utf8) override {
    ++second_password_calls;
    existing_password_bytes.assign(existing_password_utf8.bytes().begin(),
                                   existing_password_utf8.bytes().end());
    second_password_bytes.assign(second_password_utf8.bytes().begin(),
                                 second_password_utf8.bytes().end());
    existing_password_utf8.clear();
    second_password_utf8.clear();
  }

  void reset_card_record() override { ++reset_calls; }

  int reset_calls{};
  std::vector<std::byte> existing_password_bytes;
  std::vector<std::byte> second_password_bytes;
  int second_password_calls{};

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }

  std::optional<CardEntryRoute> route;
  std::vector<std::byte> initialization_bytes;
  bool reject_credential{};
  int initialization_calls{};
  int login_calls{};
};

// Selection stays native: the page can only ask, so the fixture decides which
// path comes back and an empty string stands for a cancelled dialog.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  [[nodiscard]] std::wstring choose_new_container() override {
    ++new_calls;
    return new_path;
  }

  [[nodiscard]] std::wstring choose_existing_container() override {
    ++existing_calls;
    return existing_path;
  }

  std::wstring new_path;
  std::wstring existing_path;
  int new_calls{};
  int existing_calls{};
};

struct Fixture final {
  Fixture()
      : volume(authentication),
        controller(entry, authentication, volume, policy, chooser) {}

  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume;
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller;
};

void initialize_byte_array(VARIANTARG& value,
                           const std::span<const std::byte> input) {
  ::VariantInit(&value);
  SAFEARRAY* const array =
      ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(input.size()));
  if (array == nullptr) throw std::runtime_error("SAFEARRAY allocation failed");
  value.vt = VT_ARRAY | VT_UI1;
  value.parray = array;

  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(array, &raw)),
          "SAFEARRAY access failed");
  auto* const data = static_cast<std::byte*>(raw);
  for (std::size_t index = 0; index < input.size(); ++index) {
    data[index] = input[index];
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(array)),
          "SAFEARRAY release failed");
}

void require_array_wiped(const VARIANTARG& value,
                         const std::size_t size,
                         const char* const message) {
  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(value.parray, &raw)),
          "SAFEARRAY inspection failed");
  const auto* const data = static_cast<const std::byte*>(raw);
  for (std::size_t index = 0; index < size; ++index) {
    require(data[index] == std::byte{0}, message);
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(value.parray)),
          "SAFEARRAY inspection release failed");
}

class ByteArray final {
 public:
  explicit ByteArray(const std::span<const std::byte> input)
      : size_(input.size()) {
    initialize_byte_array(value_, input);
  }

  ~ByteArray() noexcept { static_cast<void>(::VariantClear(&value_)); }

  ByteArray(const ByteArray&) = delete;
  ByteArray& operator=(const ByteArray&) = delete;

  [[nodiscard]] VARIANTARG& value() noexcept { return value_; }
  void require_wiped(const char* const message) const {
    require_array_wiped(value_, size_, message);
  }

 private:
  VARIANTARG value_{};
  std::size_t size_{};
};

class InitializationArguments final {
 public:
  InitializationArguments(const std::span<const std::byte> password,
                          const std::span<const std::byte> confirmation)
      : password_size_(password.size()),
        confirmation_size_(confirmation.size()) {
    // IDispatch receives positional arguments in reverse order.
    initialize_byte_array(values_[0], confirmation);
    initialize_byte_array(values_[1], password);
  }

  ~InitializationArguments() noexcept {
    static_cast<void>(::VariantClear(&values_[0]));
    static_cast<void>(::VariantClear(&values_[1]));
  }

  InitializationArguments(const InitializationArguments&) = delete;
  InitializationArguments& operator=(const InitializationArguments&) = delete;

  [[nodiscard]] DISPPARAMS parameters() noexcept {
    return DISPPARAMS{values_.data(), nullptr, 2, 0};
  }

  void replace_password_with_bstr() {
    static_cast<void>(::VariantClear(&values_[1]));
    values_[1].vt = VT_BSTR;
    values_[1].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[1].bstrVal != nullptr, "test BSTR allocation failed");
    password_is_array_ = false;
  }

  void replace_confirmation_with_bstr() {
    static_cast<void>(::VariantClear(&values_[0]));
    values_[0].vt = VT_BSTR;
    values_[0].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[0].bstrVal != nullptr, "test BSTR allocation failed");
    confirmation_is_array_ = false;
  }

  void require_password_wiped() const {
    require(password_is_array_, "password is not an array");
    require_array_wiped(values_[1], password_size_,
                        "initialization password SAFEARRAY survived");
  }

  void require_confirmation_wiped() const {
    require(confirmation_is_array_, "confirmation is not an array");
    require_array_wiped(values_[0], confirmation_size_,
                        "initialization confirmation SAFEARRAY survived");
  }

 private:
  std::array<VARIANTARG, 2> values_{};
  std::size_t password_size_{};
  std::size_t confirmation_size_{};
  bool password_is_array_{true};
  bool confirmation_is_array_{true};
};

class RecordingCloseSink final : public usb_vault::ui::IUiCloseRequestSink {
 public:
  void request_close() noexcept override { ++close_calls; }

  int close_calls{};
};

RecordingCloseSink g_close_sink;

[[nodiscard]] ComPtr<IDispatch> make_host(
    CardProductUiSessionController& controller) {
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              controller, &g_close_sink, &host)),
          "product host creation failed");
  return host;
}

[[nodiscard]] DISPID member_id(IDispatch& host, wchar_t* const name) {
  LPOLESTR mutable_name = name;
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &mutable_name, 1,
                                       LOCALE_INVARIANT, &id)),
          "product host member was not exposed");
  return id;
}

[[nodiscard]] DISPID member_id(IDispatch& host, const wchar_t* const name) {
  return member_id(host, const_cast<wchar_t*>(name));
}

[[nodiscard]] HRESULT invoke_login(IDispatch& host, VARIANTARG& password,
                                   UINT* const argument_error = nullptr) {
  DISPPARAMS parameters{&password, nullptr, 1, 0};
  return host.Invoke(member_id(host, L"submitLoginPasswordBytes"), IID_NULL,
                     LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                     nullptr, argument_error);
}

[[nodiscard]] HRESULT invoke_initialization(
    IDispatch& host, InitializationArguments& arguments,
    UINT* const argument_error = nullptr) {
  DISPPARAMS parameters = arguments.parameters();
  return host.Invoke(
      member_id(host, L"submitInitializationPasswordBytes"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr,
      argument_error);
}

[[nodiscard]] LONG invoke_state(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getSessionState"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "product state query failed");
  require(result.vt == VT_I4, "product state returned the wrong type");
  const LONG state = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return state;
}

[[nodiscard]] bool invoke_mounted(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"hasActiveSession"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "mounted query failed");
  require(result.vt == VT_BOOL, "mounted query returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] VARIANTARG number_argument(const std::uint32_t value) noexcept {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_UI4;
  argument.ulVal = value;
  return argument;
}

[[nodiscard]] HRESULT invoke_with_number(IDispatch& host,
                                         const wchar_t* const name,
                                         const std::uint32_t value,
                                         VARIANT* const result,
                                         UINT* const argument_error) {
  VARIANTARG argument = number_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr,
                     argument_error);
}

[[nodiscard]] ULONG invoke_container_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerCount"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container count query failed");
  require(result.vt == VT_UI4, "container count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] ULONG invoke_operation_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getOperationCount"),
                                IID_NULL,
                                LOCALE_INVARIANT,
                                DISPATCH_METHOD,
                                &parameters,
                                &result,
                                nullptr,
                                nullptr)),
          "operation count query failed");
  require(result.vt == VT_UI4, "operation count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] LONG invoke_operation_number(IDispatch& host,
                                           const wchar_t* const name,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "operation numeric fact query failed");
  require(result.vt == VT_I4, "operation numeric fact returned the wrong type");
  const LONG value = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] double invoke_operation_time(IDispatch& host,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(
              host, L"getOperationCompletedAtUnixMs", index, &result, nullptr)),
          "operation completion time query failed");
  require(result.vt == VT_R8,
          "operation completion time returned the wrong type");
  const double value = result.dblVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] ULONG invoke_container_number(IDispatch& host,
                                            const wchar_t* const name,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container fact query failed");
  require(result.vt == VT_UI4, "container fact returned the wrong type");
  const ULONG value = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_container_mounted(IDispatch& host,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, L"getContainerMounted", index,
                                       &result, nullptr)),
          "container mounted query failed");
  require(result.vt == VT_BOOL, "container mounted returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] std::wstring invoke_container_text(IDispatch& host,
                                                 const wchar_t* const name,
                                                 const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container text query failed");
  require(result.vt == VT_BSTR && result.bstrVal != nullptr,
          "container text returned the wrong type");
  const std::wstring value(result.bstrVal, ::SysStringLen(result.bstrVal));
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_selection(IDispatch& host,
                                    const wchar_t* const name,
                                    const std::uint32_t value) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, value, &result, nullptr)),
          "container selection call failed");
  require(result.vt == VT_BOOL, "container selection returned the wrong type");
  const bool selected = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return selected;
}

[[nodiscard]] bool invoke_mount_container(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"mountContainer"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "container adoption call failed");
  require(result.vt == VT_BOOL, "container adoption returned the wrong type");
  const bool adopted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return adopted;
}

void test_surface_and_state_projection_are_fixed() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  for (const auto [route, expected] : {
           std::pair{std::optional<CardEntryRoute>{}, 0L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::initialization_required},
                     1L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::login_required},
                     2L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::unavailable},
                     4L}}) {
    fixture.entry.route = route;
    require(invoke_state(*host.Get()) == expected,
            "product route mapped to the wrong wire state");
  }
  fixture.authentication.establish(root_secret());
  require(invoke_state(*host.Get()) == 3L,
          "authenticated state mapped to the wrong wire value");

  for (const wchar_t* const forbidden : {
           L"submitPasswordBytes", L"hardwareId", L"credentialRole",
           L"mode", L"tier", L"containerShape", L"volumeType",
           L"rootSecret"}) {
    LPOLESTR name = const_cast<LPOLESTR>(forbidden);
    DISPID id = 0;
    require(host->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT, &id) ==
                    DISP_E_UNKNOWNNAME &&
                id == DISPID_UNKNOWN,
            "product host exposed a forbidden member");
  }
}

void test_login_consumes_and_wipes_one_array() {
  for (const bool reject : {false, true}) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    fixture.entry.reject_credential = reject;
    fixture.volume.mount_on_login = true;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    std::vector<std::byte> password{
        std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}};
    ByteArray argument(password);

    const HRESULT result = invoke_login(*host.Get(), argument.value());
    argument.require_wiped("login SAFEARRAY survived host invocation");
    require(fixture.entry.login_calls == 1,
            "login host did not forward exactly once");
    if (reject) {
      require(result == E_FAIL && invoke_state(*host.Get()) == 2L,
              "credential rejection lost the retryable login state");
    } else {
      const std::array expected{std::byte{0xC3}, std::byte{0xA9}};
      require(result == S_OK &&
                  equals(fixture.volume.login_bytes, expected) &&
                  invoke_state(*host.Get()) == 3L &&
                  invoke_mounted(*host.Get()),
              "login host changed NFC, state, or mounted semantics");
    }
  }
}

void test_initialization_preserves_com_order_and_wipes_both_arrays() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  std::vector<std::byte> password = bytes("aaaaaaaaaaae");
  password.push_back(std::byte{0xCC});
  password.push_back(std::byte{0x81});
  std::vector<std::byte> confirmation = bytes("aaaaaaaaaaa");
  confirmation.push_back(std::byte{0xC3});
  confirmation.push_back(std::byte{0xA9});
  InitializationArguments arguments(password, confirmation);

  require(invoke_initialization(*host.Get(), arguments) == S_OK,
          "initialization host rejected equivalent NFC inputs");
  arguments.require_password_wiped();
  arguments.require_confirmation_wiped();
  std::vector<std::byte> expected = bytes("aaaaaaaaaaa");
  expected.push_back(std::byte{0xC3});
  expected.push_back(std::byte{0xA9});
  require(fixture.entry.initialization_calls == 1 &&
              equals(fixture.entry.initialization_bytes, expected) &&
              invoke_state(*host.Get()) == 3L,
          "initialization host reversed, changed, or failed inputs");
}

void test_initialization_failure_wipes_every_supported_argument() {
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("bbbbbbbbbbbb");
    InitializationArguments arguments(password, confirmation);

    require(invoke_initialization(*host.Get(), arguments) == E_FAIL,
            "mismatched initialization inputs were accepted");
    arguments.require_password_wiped();
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "mismatched inputs reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_password_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 1,
            "invalid password argument returned the wrong COM result");
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid password argument reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_confirmation_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 0,
            "invalid confirmation returned the wrong COM result");
    arguments.require_password_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid confirmation reached the card action");
  }
}

void test_bad_arity_still_wipes_supplied_password_array() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto password = bytes("aaaaaaaaaaaa");
  ByteArray argument(password);
  DISPPARAMS parameters{&argument.value(), nullptr, 1, 0};

  require(host->Invoke(member_id(*host.Get(),
                                 L"submitInitializationPasswordBytes"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "initialization host accepted the wrong argument count");
  argument.require_wiped("bad-arity password SAFEARRAY survived");
}

void test_unmount_and_property_rejection() {
  Fixture fixture;
  fixture.volume.mounted_state = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};

  require(invoke_mounted(*host.Get()), "host hid active volume state");
  require(host->Invoke(member_id(*host.Get(), L"unmount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                       nullptr, nullptr) == S_OK &&
              fixture.volume.unmount_calls == 1 &&
              !invoke_mounted(*host.Get()),
          "host did not forward unified unmount");
  require(host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_PROPERTYGET, &parameters,
                       nullptr, nullptr, nullptr) == DISP_E_MEMBERNOTFOUND,
          "product state was exposed as a property");
}

void test_container_collection_is_exposed_by_position_and_route_gated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_container_count(*host.Get()) == 0UL,
          "unauthenticated host exposed adopted containers");

  fixture.authentication.establish(root_secret());
  fixture.chooser.new_path = L"D:/Vault/work.hc";
  require(invoke_selection(*host.Get(), L"createContainerMib", 64) &&
              fixture.chooser.new_calls == 1 &&
              fixture.volume.create_calls == 1 &&
              fixture.volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL,
          "creation did not run the native chooser and the session once");

  require(invoke_container_count(*host.Get()) == 1UL &&
              invoke_container_number(*host.Get(), L"getContainerId", 0) ==
                  1UL &&
              invoke_container_text(*host.Get(), L"getContainerName", 0) ==
                  L"work.hc" &&
              invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_number(*host.Get(), L"getContainerSizeMib",
                                      0) == 64UL &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0) == L"D:",
          "host lost an adopted container fact");

  VARIANT result{};
  ::VariantInit(&result);
  require(invoke_with_number(*host.Get(), L"getContainerName", 1, &result,
                             nullptr) == E_INVALIDARG,
          "host answered a position no container occupies");
  static_cast<void>(::VariantClear(&result));
}

void test_container_calls_reject_wrong_shapes_and_report_cancellation() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  UINT argument_error = 99;
  VARIANTARG text{};
  ::VariantInit(&text);
  text.vt = VT_BSTR;
  text.bstrVal = ::SysAllocString(L"D:/Vault/injected.hc");
  require(text.bstrVal != nullptr, "test BSTR allocation failed");
  DISPPARAMS text_parameters{&text, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &text_parameters, nullptr, nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host accepted a string where only a capacity is allowed");
  static_cast<void>(::VariantClear(&text));

  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "host accepted a creation call without a capacity");
  require(invoke_with_number(*host.Get(), L"createContainerMib", 19, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity below the shared minimum");
  VARIANTARG fractional{};
  ::VariantInit(&fractional);
  fractional.vt = VT_R8;
  fractional.dblVal = 64.5;
  DISPPARAMS fractional_parameters{&fractional, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &fractional_parameters, nullptr, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity that is not a whole number");
  require(fixture.chooser.new_calls == 0 && fixture.volume.create_calls == 0,
          "a rejected creation still opened the chooser or the session");

  require(!invoke_selection(*host.Get(), L"createContainerMib", 64) &&
              fixture.chooser.new_calls == 1 &&
              fixture.volume.create_calls == 0,
          "a cancelled chooser was not reported as a cancellation");
  require(!invoke_mount_container(*host.Get()) &&
              fixture.chooser.existing_calls == 1,
          "a cancelled adoption was not reported as a cancellation");
}

void test_adopted_container_mount_and_unmount_route_through_identifiers() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  fixture.chooser.new_path = L"D:/Vault/work.hc";
  require(invoke_selection(*host.Get(), L"createContainerMib", 64),
          "creation was refused");
  const ULONG id = invoke_container_number(*host.Get(), L"getContainerId", 0);

  require(invoke_with_number(*host.Get(), L"unmountContainer", id, nullptr,
                             nullptr) == S_OK &&
              fixture.volume.unmount_container_calls == 1 &&
              !invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0).empty(),
          "unmounting by identifier did not clear the drive");

  require(invoke_selection(*host.Get(), L"mountAdoptedContainer", id) &&
              fixture.chooser.existing_calls == 0 &&
              invoke_container_mounted(*host.Get(), 0),
          "remounting by identifier asked for the file again");

  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"mountAdoptedContainer", id + 7,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host mounted an identifier the session never adopted");
  require(invoke_with_number(*host.Get(), L"unmountContainer", id + 7, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host unmounted an identifier the session never adopted");
}

void test_container_b_creation_consumes_four_reversed_arguments() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto login = bytes("login-password");
  const auto second = bytes("second-password-value");

  std::array<VARIANTARG, 4> values{};
  // COM delivers positional arguments in reverse order. The password the
  // session was opened under is no longer among them.
  initialize_byte_array(values[0], second);
  initialize_byte_array(values[1], second);
  values[2] = number_argument(16);
  values[3] = number_argument(64);
  DISPPARAMS parameters{values.data(), nullptr, 4, 0};

  const HRESULT result =
      host->Invoke(member_id(*host.Get(), L"createContainerBMib"), IID_NULL,
                   LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                   nullptr, nullptr);
  require(result == S_OK, "container B creation was refused");
  require(fixture.entry.second_password_calls == 1 &&
              fixture.entry.existing_password_bytes == login &&
              fixture.entry.second_password_bytes == second,
          "the card record did not receive both passwords");
  require(fixture.volume.create_b_calls == 1 &&
              fixture.volume.created_outer_bytes == 64ULL * 1024ULL * 1024ULL &&
              fixture.volume.created_hidden_bytes ==
                  16ULL * 1024ULL * 1024ULL,
          "the volume transaction received the wrong sizes");
  for (std::size_t index = 0; index < 2; ++index) {
    require_array_wiped(values[index], second.size(),
                        "a container B password array survived the call");
  }
  for (VARIANTARG& value : values) {
    static_cast<void>(::VariantClear(&value));
  }

  Fixture short_call;
  short_call.authentication.establish(root_secret());
  ComPtr<IDispatch> short_host = make_host(short_call.controller);
  DISPPARAMS empty{};
  require(short_host->Invoke(member_id(*short_host.Get(),
                                       L"createContainerBMib"),
                             IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                             &empty, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "container B accepted the wrong argument count");
  require(short_call.entry.second_password_calls == 0 &&
              short_call.volume.create_b_calls == 0,
          "a malformed container B call still reached the product");
}

void test_container_query_failure_does_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  fixture.volume.fail_container_statuses = true;

  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"getContainerCount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &result,
                       nullptr, nullptr) == E_FAIL,
          "a failed container query reported a count anyway");
  require(invoke_with_number(*host.Get(), L"getContainerName", 0, &result,
                             nullptr) == E_FAIL,
          "a failed container query reported a name anyway");
  static_cast<void>(::VariantClear(&result));
}

void test_operation_history_is_read_only_typed_and_clearable() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_operation_count(*host.Get()) == 0,
          "an unauthenticated route exposed operation history");

  fixture.authentication.establish(root_secret());
  fixture.volume.operation_events = {
      {.sequence = 1,
       .completed_at_unix_ms = 1788253200000,
       .kind = usb_vault::application::OperationKind::session_opened,
       .outcome = usb_vault::application::OperationOutcome::succeeded},
      {.sequence = 2,
       .completed_at_unix_ms = 1788253201000,
       .kind = usb_vault::application::OperationKind::container_mounted,
       .outcome = usb_vault::application::OperationOutcome::succeeded,
       .container_name = L"work.hc",
       .drive_letter = L'X'},
  };
  require(
      invoke_operation_count(*host.Get()) == 2 &&
          invoke_operation_time(*host.Get(), 1) == 1788253201000.0 &&
          invoke_operation_number(*host.Get(), L"getOperationKind", 1) == 2 &&
          invoke_operation_number(*host.Get(), L"getOperationOutcome", 1) ==
              0 &&
          invoke_container_text(*host.Get(), L"getOperationContainerName", 1) ==
              L"work.hc" &&
          invoke_container_text(*host.Get(), L"getOperationDriveLetter", 1) ==
              L"X:",
      "operation history lost a typed native fact");

  DISPPARAMS no_arguments{};
  require(host->Invoke(member_id(*host.Get(), L"clearOperationHistory"),
                       IID_NULL,
                       LOCALE_INVARIANT,
                       DISPATCH_METHOD,
                       &no_arguments,
                       nullptr,
                       nullptr,
                       nullptr) == S_OK &&
              fixture.volume.clear_operation_history_calls == 1 &&
              invoke_operation_count(*host.Get()) == 0,
          "the clear-history action did not remove the current-session facts");
}

void test_operation_history_query_failures_do_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.fail_operation_history = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(
      host->Invoke(member_id(*host.Get(), L"getOperationCount"),
                   IID_NULL,
                   LOCALE_INVARIANT,
                   DISPATCH_METHOD,
                   &parameters,
                   &result,
                   nullptr,
                   nullptr) == E_FAIL &&
          invoke_with_number(
              *host.Get(), L"getOperationKind", 0, &result, nullptr) == E_FAIL,
      "a failed history query returned a substitute fact");
  static_cast<void>(::VariantClear(&result));
}

}  // namespace

int main() {
  try {
    test_surface_and_state_projection_are_fixed();
    test_login_consumes_and_wipes_one_array();
    test_initialization_preserves_com_order_and_wipes_both_arrays();
    test_initialization_failure_wipes_every_supported_argument();
    test_bad_arity_still_wipes_supplied_password_array();
    test_unmount_and_property_rejection();
    test_container_collection_is_exposed_by_position_and_route_gated();
    test_container_calls_reject_wrong_shapes_and_report_cancellation();
    test_adopted_container_mount_and_unmount_route_through_identifiers();
    test_container_b_creation_consumes_four_reversed_arguments();
    test_container_query_failure_does_not_invent_facts();
    test_operation_history_is_read_only_typed_and_clearable();
    test_operation_history_query_failures_do_not_invent_facts();
    std::cout << "card_product_webview_host_object_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_webview_host_object_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
