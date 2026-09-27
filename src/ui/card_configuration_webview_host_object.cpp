#include "usb_vault/ui/card_configuration_webview_host_object.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "usb_vault/ui/page_event_report.h"

#include "usb_vault/security/secure_buffer.h"
#include "usb_vault/ui/webview_password_array.h"

#include <Windows.h>

#include <atomic>
#include <new>
#include <stdexcept>

namespace usb_vault::ui {
namespace {

constexpr DISPID kGetStartupViewId = 1;
constexpr DISPID kGetCredentialStatusId = 2;
constexpr DISPID kSaveCredentialBytesId = 3;
constexpr DISPID kContinueToSessionId = 4;
constexpr DISPID kReportPageEventId = 5;

constexpr wchar_t kConfigurationView[] = L"configuration";

// Setup accepts exactly one three-value set, so every argument is guarded
// before any of them is inspected: a failure on the first value must not
// leave the other two readable in the array the page supplied.
class CredentialArgumentsWiper final {
 public:
  CredentialArgumentsWiper(VARIANT& first, VARIANT& second,
                           VARIANT& third) noexcept
      : first_(first), second_(second), third_(third) {}

  ~CredentialArgumentsWiper() noexcept {
    wipe_webview_password_array(first_);
    wipe_webview_password_array(second_);
    wipe_webview_password_array(third_);
  }

  CredentialArgumentsWiper(const CredentialArgumentsWiper&) = delete;
  CredentialArgumentsWiper& operator=(const CredentialArgumentsWiper&) = delete;
  CredentialArgumentsWiper(CredentialArgumentsWiper&&) = delete;
  CredentialArgumentsWiper& operator=(CredentialArgumentsWiper&&) = delete;

 private:
  VARIANT& first_;
  VARIANT& second_;
  VARIANT& third_;
};

class CardConfigurationWebViewHostObject final : public IDispatch {
 public:
  explicit CardConfigurationWebViewHostObject(
      IConfigurationHost& host) noexcept
      : host_(host) {}

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
    if (matches(names[0], L"reportPageEvent")) { ids[0] = kReportPageEventId; return S_OK; }
    if (matches(names[0], L"getStartupView")) {
      ids[0] = kGetStartupViewId;
      return S_OK;
    }
    if (matches(names[0], L"getCredentialStatus")) {
      ids[0] = kGetCredentialStatusId;
      return S_OK;
    }
    if (matches(names[0], L"saveCredentialBytes")) {
      ids[0] = kSaveCredentialBytesId;
      return S_OK;
    }
    if (matches(names[0], L"continueToSession")) {
      ids[0] = kContinueToSessionId;
      return S_OK;
    }
    ids[0] = DISPID_UNKNOWN;
    return DISP_E_UNKNOWNNAME;
  }

  HRESULT STDMETHODCALLTYPE Invoke(DISPID member, REFIID iid, LCID, WORD flags,
                                   DISPPARAMS* parameters, VARIANT* result,
                                   EXCEPINFO*, UINT* argument_error) noexcept
      override {
    if (member == kReportPageEventId)
      return report_page_event(iid, flags, parameters, result, argument_error);
    if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
    // A credential is expected as a byte array, but one that arrives as a
    // string was marshalled into this process all the same, and the caller
    // frees it without clearing it.
    const DispatchTextWiper text_wiper(parameters);
    if ((flags & DISPATCH_METHOD) == 0) return DISP_E_MEMBERNOTFOUND;
    // Once the product session has been requested this page is retired. It
    // keeps running until the host tears it down, so it must not still be able
    // to store credentials the starting session would never read.
    if (session_requested_) {
      wipe_available_arguments(parameters);
      return E_PENDING;
    }
    switch (member) {
      case kGetStartupViewId:
        if (!has_no_arguments(parameters)) return reject_arguments(parameters);
        if (result == nullptr) return E_POINTER;
        ::VariantInit(result);
        result->vt = VT_BSTR;
        result->bstrVal = ::SysAllocString(kConfigurationView);
        return result->bstrVal != nullptr ? S_OK : E_OUTOFMEMORY;
      case kGetCredentialStatusId:
        if (!has_no_arguments(parameters)) return reject_arguments(parameters);
        if (result == nullptr) return E_POINTER;
        ::VariantInit(result);
        result->vt = VT_UI4;
        result->ulVal = static_cast<ULONG>(
            static_cast<unsigned int>(host_.credential_status()));
        return S_OK;
      case kSaveCredentialBytesId:
        return invoke_save(parameters, result, argument_error);
      case kContinueToSessionId:
        if (!has_no_arguments(parameters)) return reject_arguments(parameters);
        if (!host_.continue_to_session()) return E_ACCESSDENIED;
        session_requested_ = true;
        if (result != nullptr) {
          ::VariantInit(result);
          result->vt = VT_BOOL;
          result->boolVal = VARIANT_TRUE;
        }
        return S_OK;
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

  [[nodiscard]] static HRESULT reject_arguments(
      DISPPARAMS* const parameters) noexcept {
    wipe_available_arguments(parameters);
    return DISP_E_BADPARAMCOUNT;
  }

  static void wipe_available_arguments(DISPPARAMS* const parameters) noexcept {
    if (parameters == nullptr || parameters->rgvarg == nullptr) return;
    for (UINT index = 0; index < parameters->cArgs; ++index) {
      wipe_webview_password_array(parameters->rgvarg[index]);
    }
  }

  [[nodiscard]] static HRESULT complete_outcome(
      const ConfigurationOutcome outcome, VARIANT* const result) noexcept {
    ::VariantInit(result);
    result->vt = VT_UI4;
    result->ulVal = static_cast<ULONG>(static_cast<unsigned int>(outcome));
    return S_OK;
  }

  HRESULT invoke_save(DISPPARAMS* const parameters, VARIANT* const result,
                      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 3 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return reject_arguments(parameters);
    }
    // Checked before the set is stored: a page that cannot be told the
    // outcome must not have its credentials written anyway.
    if (result == nullptr) {
      wipe_available_arguments(parameters);
      return E_POINTER;
    }
    // COM stores positional arguments in reverse order.
    VARIANT& user_pin_argument = parameters->rgvarg[0];
    VARIANT& api_key_argument = parameters->rgvarg[1];
    VARIANT& user_sn_argument = parameters->rgvarg[2];
    CredentialArgumentsWiper source_wiper(user_sn_argument, api_key_argument,
                                          user_pin_argument);
    security::SecureBuffer user_sn;
    security::SecureBuffer api_key;
    security::SecureBuffer user_pin;
    if (const HRESULT consumed =
            consume(user_sn_argument, user_sn, 2, argument_error);
        FAILED(consumed)) {
      return consumed;
    }
    if (const HRESULT consumed =
            consume(api_key_argument, api_key, 1, argument_error);
        FAILED(consumed)) {
      return consumed;
    }
    if (const HRESULT consumed =
            consume(user_pin_argument, user_pin, 0, argument_error);
        FAILED(consumed)) {
      return consumed;
    }
    return complete_outcome(
        host_.save_credentials(user_sn.bytes(), api_key.bytes(),
                               user_pin.bytes()),
        result);
  }

  [[nodiscard]] static HRESULT consume(VARIANT& argument,
                                       security::SecureBuffer& value,
                                       const UINT position,
                                       UINT* const argument_error) noexcept {
    try {
      value = consume_webview_password_array(argument);
      return S_OK;
    } catch (const std::invalid_argument&) {
      USB_VAULT_TRACE_FAILURE("ui/card_configuration_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = position;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_configuration_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = position;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_configuration_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  IConfigurationHost& host_;
  // WebView2 marshals this object, and its own reference may be released off
  // the thread that created it. A torn count would either leak the bridge or
  // delete it while a call is running.
  std::atomic<ULONG> references_{1};
  bool session_requested_{false};
};

}  // namespace

HRESULT create_card_configuration_webview_host_object(
    IConfigurationHost& host, IDispatch** const result) noexcept {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  auto* const object =
      new (std::nothrow) CardConfigurationWebViewHostObject(host);
  if (object == nullptr) return E_OUTOFMEMORY;
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
