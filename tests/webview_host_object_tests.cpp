#include "usb_vault/ui/webview_host_object.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

using Microsoft::WRL::ComPtr;
using usb_vault::application::IApplicationSessionActions;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::UiSessionController;
using usb_vault::ui::create_webview_host_object;

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class FakeSession final : public IApplicationSessionActions {
 public:
  void authenticate_and_mount(SecureBuffer password) override {
    ++calls;
    require(password.size() == expected.size(), "host changed password size");
    for (std::size_t index = 0; index < expected.size(); ++index) {
      require(password.bytes()[index] == expected[index],
              "host changed password bytes");
    }
    if (reject) {
      throw std::runtime_error("rejected");
    }
    is_mounted = true;
  }
  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) {
      throw std::runtime_error("unmount rejected");
    }
    is_mounted = false;
  }
  [[nodiscard]] bool mounted() const noexcept override { return is_mounted; }

  std::array<std::byte, 4> expected{
      std::byte{'p'}, std::byte{0}, std::byte{0xC3}, std::byte{0xA4}};
  bool reject{false};
  bool reject_unmount{false};
  bool is_mounted{false};
  int calls{0};
  int unmount_calls{0};
};

class ByteArray final {
 public:
  explicit ByteArray(const std::array<std::byte, 4>& bytes) {
    ::VariantInit(&value_);
    SAFEARRAY* const array =
        ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(bytes.size()));
    if (array == nullptr) {
      throw std::runtime_error("SAFEARRAY allocation failed");
    }
    value_.vt = VT_ARRAY | VT_UI1;
    value_.parray = array;
    void* raw = nullptr;
    require(SUCCEEDED(::SafeArrayAccessData(array, &raw)),
            "SAFEARRAY access failed");
    auto* const data = static_cast<std::byte*>(raw);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      data[index] = bytes[index];
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(array)),
            "SAFEARRAY release failed");
  }

  ~ByteArray() noexcept { static_cast<void>(::VariantClear(&value_)); }
  [[nodiscard]] VARIANTARG& value() noexcept { return value_; }

  void require_wiped() const {
    void* raw = nullptr;
    require(SUCCEEDED(::SafeArrayAccessData(value_.parray, &raw)),
            "SAFEARRAY inspection failed");
    const auto* const data = static_cast<const std::byte*>(raw);
    for (std::size_t index = 0; index < 4; ++index) {
      require(data[index] == std::byte{0}, "host did not wipe SAFEARRAY");
    }
    require(SUCCEEDED(::SafeArrayUnaccessData(value_.parray)),
            "SAFEARRAY inspection release failed");
  }

 private:
  VARIANTARG value_{};
};

ComPtr<IDispatch> make_host(UiSessionController& controller) {
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_webview_host_object(controller, &host)),
          "host creation failed");
  return host;
}

DISPID submit_id(IDispatch& host) {
  LPOLESTR name = const_cast<LPOLESTR>(L"submitPasswordBytes");
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT,
                                      &id)),
          "submit method was not exposed");
  return id;
}

DISPID unmount_id(IDispatch& host) {
  LPOLESTR name = const_cast<LPOLESTR>(L"unmount");
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT,
                                      &id)),
          "unmount method was not exposed");
  return id;
}

DISPID has_active_session_id(IDispatch& host) {
  LPOLESTR name = const_cast<LPOLESTR>(L"hasActiveSession");
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT,
                                      &id)),
          "active-session method was not exposed");
  return id;
}

HRESULT invoke_submit(IDispatch& host, VARIANTARG& password) {
  DISPPARAMS parameters{&password, nullptr, 1, 0};
  return host.Invoke(submit_id(host), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, nullptr, nullptr, nullptr);
}

HRESULT invoke_unmount(IDispatch& host) {
  DISPPARAMS parameters{};
  return host.Invoke(unmount_id(host), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, nullptr, nullptr, nullptr);
}

bool invoke_has_active_session(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(has_active_session_id(host), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "active-session query failed");
  require(result.vt == VT_BOOL,
          "active-session query returned the wrong type");
  const bool active = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return active;
}

void test_narrow_automation_surface() {
  FakeSession session;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);

  UINT type_info_count = 1;
  require(SUCCEEDED(host->GetTypeInfoCount(&type_info_count)) &&
              type_info_count == 0,
          "host exposed type information");
  LPOLESTR unknown = const_cast<LPOLESTR>(L"mounted");
  DISPID id = 0;
  require(host->GetIDsOfNames(IID_NULL, &unknown, 1, LOCALE_INVARIANT, &id) ==
              DISP_E_UNKNOWNNAME &&
              id == DISPID_UNKNOWN,
          "host exposed unexpected state");
}

void test_submit_and_failure_wipe() {
  for (const bool reject : {false, true}) {
    FakeSession session;
    session.reject = reject;
    UiSessionController controller(session);
    ComPtr<IDispatch> host = make_host(controller);
    ByteArray password(session.expected);

    const HRESULT result = invoke_submit(*host.Get(), password.value());
    password.require_wiped();
    require(session.calls == 1, "host did not forward submission");
    require(reject ? result == E_FAIL : result == S_OK,
            "host returned the wrong submission result");
  }
}

void test_wrong_argument_is_rejected() {
  FakeSession session;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_BSTR;
  argument.bstrVal = ::SysAllocString(L"must not be accepted");
  require(argument.bstrVal != nullptr, "test BSTR allocation failed");

  const HRESULT result = invoke_submit(*host.Get(), argument);
  static_cast<void>(::VariantClear(&argument));
  require(result == DISP_E_TYPEMISMATCH,
          "host accepted a string password argument");
  require(session.calls == 0, "wrong argument reached the session");
}

void test_unmount_success_and_failure() {
  for (const bool reject : {false, true}) {
    FakeSession session;
    session.is_mounted = true;
    session.reject_unmount = reject;
    UiSessionController controller(session);
    ComPtr<IDispatch> host = make_host(controller);

    const HRESULT result = invoke_unmount(*host.Get());
    require(session.unmount_calls == 1, "host did not forward unmount");
    require(reject ? result == E_FAIL : result == S_OK,
            "host returned the wrong unmount result");
    require(session.is_mounted == reject,
            "host reported the wrong unmount state through the fake session");
  }
}

void test_unmount_argument_is_rejected() {
  FakeSession session;
  session.is_mounted = true;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_UI1;
  argument.bVal = 1;
  DISPPARAMS parameters{&argument, nullptr, 1, 0};

  require(host->Invoke(unmount_id(*host.Get()), IID_NULL, LOCALE_INVARIANT,
                       DISPATCH_METHOD, &parameters, nullptr, nullptr,
                       nullptr) == DISP_E_BADPARAMCOUNT,
          "unmount accepted an argument");
  require(session.unmount_calls == 0,
          "invalid unmount invocation reached the session");
}

void test_unified_active_session_query() {
  FakeSession session;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);

  require(!invoke_has_active_session(*host.Get()),
          "inactive session was reported active");
  session.is_mounted = true;
  require(invoke_has_active_session(*host.Get()),
          "active session was reported inactive");
}

void test_active_session_query_argument_is_rejected() {
  FakeSession session;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_UI1;
  argument.bVal = 1;
  DISPPARAMS parameters{&argument, nullptr, 1, 0};

  require(host->Invoke(has_active_session_id(*host.Get()), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                       nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "active-session query accepted an argument");
}

void test_property_access_is_rejected() {
  FakeSession session;
  UiSessionController controller(session);
  ComPtr<IDispatch> host = make_host(controller);
  DISPPARAMS parameters{};
  require(host->Invoke(submit_id(*host.Get()), IID_NULL, LOCALE_INVARIANT,
                       DISPATCH_PROPERTYGET, &parameters, nullptr, nullptr,
                       nullptr) == DISP_E_MEMBERNOTFOUND,
          "submit method was exposed as a property");
  require(host->Invoke(unmount_id(*host.Get()), IID_NULL, LOCALE_INVARIANT,
                       DISPATCH_PROPERTYGET, &parameters, nullptr, nullptr,
                       nullptr) == DISP_E_MEMBERNOTFOUND,
          "unmount method was exposed as a property");
  require(host->Invoke(has_active_session_id(*host.Get()), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_PROPERTYGET, &parameters,
                       nullptr, nullptr, nullptr) == DISP_E_MEMBERNOTFOUND,
          "active-session method was exposed as a property");
}

}  // namespace

int main() {
  test_narrow_automation_surface();
  test_submit_and_failure_wipe();
  test_wrong_argument_is_rejected();
  test_unmount_success_and_failure();
  test_unmount_argument_is_rejected();
  test_unified_active_session_query();
  test_active_session_query_argument_is_rejected();
  test_property_access_is_rejected();
  std::cout << "webview_host_object_tests: PASS\n";
  return 0;
}
