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

  std::uint32_t create_container(std::wstring_view, std::uint64_t) override {
    throw std::logic_error("the container collection is not part of this slice");
  }
  std::uint32_t mount_container(std::wstring_view) override {
    throw std::logic_error("the container collection is not part of this slice");
  }
  void unmount_container(std::uint32_t) override {
    throw std::logic_error("the container collection is not part of this slice");
  }
  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    throw std::logic_error("the container collection is not part of this slice");
  }

  [[nodiscard]] ContainerAStatus container_a_status() override {
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_size_bytes,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  void consume_authenticated_login(
      SecureBuffer root, SecureBuffer login_password_utf8) override {
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

struct Fixture final {
  Fixture()
      : volume(authentication),
        controller(entry, authentication, volume, policy) {}

  FakeEntry entry;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume;
  OfflineCardPasswordPolicy policy;
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

[[nodiscard]] ComPtr<IDispatch> make_host(
    CardProductUiSessionController& controller) {
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(controller,
                                                             &host)),
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

[[nodiscard]] HRESULT invoke_create_a(
    IDispatch& host, VARIANTARG& size,
    UINT* const argument_error = nullptr) {
  DISPPARAMS parameters{&size, nullptr, 1, 0};
  return host.Invoke(member_id(host, L"createContainerAMib"), IID_NULL,
                     LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                     nullptr, argument_error);
}

[[nodiscard]] LONG invoke_container_a_state(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerAState"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container A state query failed");
  require(result.vt == VT_I4,
          "container A state query returned the wrong type");
  const LONG state = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return state;
}

[[nodiscard]] ULONG invoke_container_a_size(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerASizeMib"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container A capacity query failed");
  require(result.vt == VT_UI4,
          "container A capacity query returned the wrong type");
  const ULONG size = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return size;
}

[[nodiscard]] std::wstring invoke_container_a_drive(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerADriveLetter"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container A drive query failed");
  require(result.vt == VT_BSTR && result.bstrVal != nullptr,
          "container A drive query returned the wrong type");
  const std::wstring drive(result.bstrVal, ::SysStringLen(result.bstrVal));
  static_cast<void>(::VariantClear(&result));
  return drive;
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

void test_container_a_creation_accepts_only_exact_bounded_numbers() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  VARIANTARG size{};
  ::VariantInit(&size);
  size.vt = VT_R8;
  size.dblVal = 64.0;
  require(invoke_create_a(*host.Get(), size) == S_OK &&
              fixture.volume.create_a_calls == 1 &&
              fixture.volume.created_a_size_bytes ==
                  64ULL * 1024ULL * 1024ULL &&
              invoke_mounted(*host.Get()),
          "host did not forward exact container A capacity");

  for (const double invalid : {19.0, 1048577.0, 64.5}) {
    size.dblVal = invalid;
    UINT argument_error = 99;
    require(invoke_create_a(*host.Get(), size, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 0,
            "host accepted an invalid container A capacity");
  }
  require(fixture.volume.create_a_calls == 1,
          "invalid capacity reached the volume session");

  size.vt = VT_BSTR;
  size.bstrVal = ::SysAllocString(L"64");
  require(size.bstrVal != nullptr, "BSTR allocation failed");
  UINT argument_error = 99;
  require(invoke_create_a(*host.Get(), size, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host coerced a string capacity");
  static_cast<void>(::VariantClear(&size));
}

void test_container_a_creation_preserves_route_and_failure_semantics() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  VARIANTARG size{};
  ::VariantInit(&size);
  size.vt = VT_UI4;
  size.ulVal = 64;

  require(invoke_create_a(*host.Get(), size) == E_FAIL &&
              fixture.volume.create_a_calls == 0,
          "host allowed creation outside authentication");

  fixture.authentication.establish(root_secret());
  fixture.volume.reject_create_a = true;
  require(invoke_create_a(*host.Get(), size) == E_FAIL &&
              fixture.volume.create_a_calls == 1 &&
              invoke_state(*host.Get()) == 3L &&
              !invoke_mounted(*host.Get()),
          "host hid creation failure or changed authentication state");

  DISPPARAMS no_arguments{};
  require(host->Invoke(member_id(*host.Get(), L"createContainerAMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &no_arguments, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "host accepted missing creation capacity");
}

void test_container_a_display_facts_are_narrow_and_route_gated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_container_a_state(*host.Get()) == 0L &&
              invoke_container_a_size(*host.Get()) == 0UL &&
              invoke_container_a_drive(*host.Get()).empty(),
          "unauthenticated host exposed container A facts");

  fixture.authentication.establish(root_secret());
  fixture.volume.container_a_exists = true;
  fixture.volume.container_a_size_bytes = 96ULL * 1024ULL * 1024ULL;
  require(invoke_container_a_state(*host.Get()) == 1L &&
              invoke_container_a_size(*host.Get()) == 96UL &&
              invoke_container_a_drive(*host.Get()).empty(),
          "host lost present, unmounted container A facts");

  fixture.volume.container_a_mounted = true;
  require(invoke_container_a_state(*host.Get()) == 2L &&
              invoke_container_a_size(*host.Get()) == 96UL &&
              invoke_container_a_drive(*host.Get()) == L"X:",
          "host did not expose mounted A capacity and fixed drive");
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
    test_container_a_creation_accepts_only_exact_bounded_numbers();
    test_container_a_creation_preserves_route_and_failure_semantics();
    test_container_a_display_facts_are_narrow_and_route_gated();
    std::cout << "card_product_webview_host_object_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_webview_host_object_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
