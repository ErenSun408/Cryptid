#include "usb_vault/ui/card_product_webview_host_object.h"

#include "usb_vault/ui/webview_password_array.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>

namespace usb_vault::ui {
namespace {

constexpr DISPID kSubmitLoginPasswordBytesId = 1;
constexpr DISPID kSubmitInitializationPasswordBytesId = 2;
constexpr DISPID kUnmountId = 3;
constexpr DISPID kHasActiveSessionId = 4;
constexpr DISPID kGetSessionStateId = 5;
constexpr DISPID kCreateContainerAMibId = 6;
constexpr wchar_t kSubmitLoginPasswordBytesName[] =
    L"submitLoginPasswordBytes";
constexpr wchar_t kSubmitInitializationPasswordBytesName[] =
    L"submitInitializationPasswordBytes";
constexpr wchar_t kUnmountName[] = L"unmount";
constexpr wchar_t kHasActiveSessionName[] = L"hasActiveSession";
constexpr wchar_t kGetSessionStateName[] = L"getSessionState";
constexpr wchar_t kCreateContainerAMibName[] = L"createContainerAMib";

class PasswordArgumentsWiper final {
 public:
  PasswordArgumentsWiper(VARIANT& first, VARIANT& second) noexcept
      : first_(first), second_(second) {}
  ~PasswordArgumentsWiper() noexcept {
    wipe_webview_password_array(first_);
    wipe_webview_password_array(second_);
  }

  PasswordArgumentsWiper(const PasswordArgumentsWiper&) = delete;
  PasswordArgumentsWiper& operator=(const PasswordArgumentsWiper&) = delete;

 private:
  VARIANT& first_;
  VARIANT& second_;
};

[[nodiscard]] LONG state_wire_value(
    const CardProductUiState state) noexcept {
  switch (state) {
    case CardProductUiState::no_card:
      return 0;
    case CardProductUiState::initialization_required:
      return 1;
    case CardProductUiState::login_required:
      return 2;
    case CardProductUiState::authenticated:
      return 3;
    case CardProductUiState::unavailable:
    default:
      return 4;
  }
}

class CardProductWebViewHostObject final : public IDispatch {
 public:
  explicit CardProductWebViewHostObject(
      CardProductUiSessionController& controller) noexcept
      : controller_(controller) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** result) noexcept override {
    if (result == nullptr) return E_POINTER;
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDispatch) {
      *result = static_cast<IDispatch*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }

  ULONG STDMETHODCALLTYPE Release() noexcept override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) noexcept override {
    if (count == nullptr) return E_POINTER;
    *count = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID,
                                        ITypeInfo**) noexcept override {
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID iid, LPOLESTR* names,
                                          UINT name_count, LCID,
                                          DISPID* ids) noexcept override {
    if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
    if (names == nullptr || ids == nullptr) return E_POINTER;
    if (name_count != 1 || names[0] == nullptr) {
      if (name_count > 0) ids[0] = DISPID_UNKNOWN;
      return DISP_E_UNKNOWNNAME;
    }
    if (matches(names[0], kSubmitLoginPasswordBytesName)) {
      ids[0] = kSubmitLoginPasswordBytesId;
      return S_OK;
    }
    if (matches(names[0], kSubmitInitializationPasswordBytesName)) {
      ids[0] = kSubmitInitializationPasswordBytesId;
      return S_OK;
    }
    if (matches(names[0], kUnmountName)) {
      ids[0] = kUnmountId;
      return S_OK;
    }
    if (matches(names[0], kHasActiveSessionName)) {
      ids[0] = kHasActiveSessionId;
      return S_OK;
    }
    if (matches(names[0], kGetSessionStateName)) {
      ids[0] = kGetSessionStateId;
      return S_OK;
    }
    if (matches(names[0], kCreateContainerAMibName)) {
      ids[0] = kCreateContainerAMibId;
      return S_OK;
    }
    ids[0] = DISPID_UNKNOWN;
    return DISP_E_UNKNOWNNAME;
  }

  HRESULT STDMETHODCALLTYPE Invoke(DISPID member, REFIID iid, LCID, WORD flags,
                                   DISPPARAMS* parameters, VARIANT* result,
                                   EXCEPINFO*, UINT* argument_error) noexcept
      override {
    if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
    if ((flags & DISPATCH_METHOD) == 0) return DISP_E_MEMBERNOTFOUND;

    switch (member) {
      case kGetSessionStateId:
        return invoke_state(parameters, result);
      case kHasActiveSessionId:
        return invoke_mounted(parameters, result);
      case kUnmountId:
        return invoke_unmount(parameters);
      case kSubmitLoginPasswordBytesId:
        return invoke_login(parameters, argument_error);
      case kSubmitInitializationPasswordBytesId:
        return invoke_initialization(parameters, argument_error);
      case kCreateContainerAMibId:
        return invoke_create_container_a(parameters, argument_error);
      default:
        return DISP_E_MEMBERNOTFOUND;
    }
  }

 private:
  [[nodiscard]] static bool matches(const wchar_t* const actual,
                                    const wchar_t* const expected) noexcept {
    return ::CompareStringOrdinal(actual, -1, expected, -1, FALSE) ==
           CSTR_EQUAL;
  }

  [[nodiscard]] static bool has_no_arguments(
      const DISPPARAMS* const parameters) noexcept {
    return parameters != nullptr && parameters->cArgs == 0 &&
           parameters->cNamedArgs == 0;
  }

  static void wipe_available_arguments(
      DISPPARAMS* const parameters, const UINT maximum) noexcept {
    if (parameters == nullptr || parameters->rgvarg == nullptr) return;
    const UINT count = (std::min)(parameters->cArgs, maximum);
    for (UINT index = 0; index < count; ++index) {
      wipe_webview_password_array(parameters->rgvarg[index]);
    }
  }

  HRESULT invoke_state(DISPPARAMS* const parameters,
                       VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (result != nullptr) {
      ::VariantInit(result);
      result->vt = VT_I4;
      result->lVal = state_wire_value(controller_.state());
    }
    return S_OK;
  }

  HRESULT invoke_mounted(DISPPARAMS* const parameters,
                         VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (result != nullptr) {
      ::VariantInit(result);
      result->vt = VT_BOOL;
      result->boolVal =
          controller_.mounted() ? VARIANT_TRUE : VARIANT_FALSE;
    }
    return S_OK;
  }

  HRESULT invoke_unmount(DISPPARAMS* const parameters) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      controller_.unmount();
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  [[nodiscard]] static bool exact_size_mib(
      const VARIANTARG& argument,
      std::uint32_t& result) noexcept {
    if (argument.vt == VT_I4 && argument.lVal >= 0) {
      result = static_cast<std::uint32_t>(argument.lVal);
      return true;
    }
    if (argument.vt == VT_UI4) {
      result = argument.ulVal;
      return true;
    }
    if (argument.vt == VT_R8 && std::isfinite(argument.dblVal) &&
        argument.dblVal >= 0.0 &&
        argument.dblVal <=
            static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) &&
        std::floor(argument.dblVal) == argument.dblVal) {
      result = static_cast<std::uint32_t>(argument.dblVal);
      return true;
    }
    return false;
  }

  HRESULT invoke_create_container_a(
      DISPPARAMS* const parameters,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t size_mib = 0;
    if (!exact_size_mib(parameters->rgvarg[0], size_mib)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.create_container_a(size_mib);
      return S_OK;
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_login(DISPPARAMS* const parameters,
                       UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      wipe_available_arguments(parameters, 1);
      return DISP_E_BADPARAMCOUNT;
    }
    security::SecureBuffer password;
    try {
      password = consume_webview_password_array(parameters->rgvarg[0]);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
    try {
      controller_.submit_login(password.bytes());
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_initialization(DISPPARAMS* const parameters,
                                UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 2 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      wipe_available_arguments(parameters, 2);
      return DISP_E_BADPARAMCOUNT;
    }

    // COM stores positional arguments in reverse order. Both sources are
    // guarded before inspecting either one so a first-argument failure cannot
    // leave the other password array intact.
    VARIANT& confirmation_argument = parameters->rgvarg[0];
    VARIANT& password_argument = parameters->rgvarg[1];
    PasswordArgumentsWiper source_wiper(password_argument,
                                         confirmation_argument);
    security::SecureBuffer password;
    try {
      password = consume_webview_password_array(password_argument);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 1;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      if (argument_error != nullptr) *argument_error = 1;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }

    security::SecureBuffer confirmation;
    try {
      confirmation =
          consume_webview_password_array(confirmation_argument);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }

    try {
      controller_.submit_initialization(password.bytes(),
                                        confirmation.bytes());
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  std::atomic<ULONG> references_{1};
  CardProductUiSessionController& controller_;
};

}  // namespace

HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IDispatch** const result) noexcept {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  auto* const object =
      new (std::nothrow) CardProductWebViewHostObject(controller);
  if (object == nullptr) return E_OUTOFMEMORY;
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
