#include "usb_vault/ui/webview_host_object.h"

#include "usb_vault/ui/webview_password_array.h"

#include <Windows.h>

#include <atomic>
#include <new>
#include <stdexcept>

namespace usb_vault::ui {
namespace {

constexpr DISPID kSubmitPasswordBytesId = 1;
constexpr DISPID kUnmountId = 2;
constexpr wchar_t kSubmitPasswordBytesName[] = L"submitPasswordBytes";
constexpr wchar_t kUnmountName[] = L"unmount";

class WebViewHostObject final : public IDispatch {
 public:
  explicit WebViewHostObject(UiSessionController& controller) noexcept
      : controller_(controller) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                           void** result) noexcept override {
    if (result == nullptr) {
      return E_POINTER;
    }
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
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }

  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) noexcept override {
    if (count == nullptr) {
      return E_POINTER;
    }
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
    if (iid != IID_NULL) {
      return DISP_E_UNKNOWNINTERFACE;
    }
    if (names == nullptr || ids == nullptr) {
      return E_POINTER;
    }
    if (name_count != 1 || names[0] == nullptr) {
      if (name_count > 0) {
        ids[0] = DISPID_UNKNOWN;
      }
      return DISP_E_UNKNOWNNAME;
    }
    if (::CompareStringOrdinal(names[0], -1, kSubmitPasswordBytesName, -1,
                               FALSE) == CSTR_EQUAL) {
      ids[0] = kSubmitPasswordBytesId;
      return S_OK;
    }
    if (::CompareStringOrdinal(names[0], -1, kUnmountName, -1, FALSE) ==
        CSTR_EQUAL) {
      ids[0] = kUnmountId;
      return S_OK;
    }
    ids[0] = DISPID_UNKNOWN;
    return DISP_E_UNKNOWNNAME;
  }

  HRESULT STDMETHODCALLTYPE Invoke(DISPID member, REFIID iid, LCID, WORD flags,
                                   DISPPARAMS* parameters, VARIANT*, EXCEPINFO*,
                                   UINT* argument_error) noexcept override {
    if (iid != IID_NULL) {
      return DISP_E_UNKNOWNINTERFACE;
    }
    if (member != kSubmitPasswordBytesId && member != kUnmountId) {
      return DISP_E_MEMBERNOTFOUND;
    }
    if ((flags & DISPATCH_METHOD) == 0) {
      return DISP_E_MEMBERNOTFOUND;
    }

    if (member == kUnmountId) {
      if (parameters == nullptr || parameters->cArgs != 0 ||
          parameters->cNamedArgs != 0) {
        return DISP_E_BADPARAMCOUNT;
      }
      try {
        controller_.unmount();
        return S_OK;
      } catch (...) {
        return E_FAIL;
      }
    }

    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }

    try {
      submit_webview_password_array(parameters->rgvarg[0], controller_);
      return S_OK;
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) {
        *argument_error = 0;
      }
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  std::atomic<ULONG> references_{1};
  UiSessionController& controller_;
};

}  // namespace

HRESULT create_webview_host_object(UiSessionController& controller,
                                   IDispatch** const result) noexcept {
  if (result == nullptr) {
    return E_POINTER;
  }
  *result = nullptr;
  auto* const object = new (std::nothrow) WebViewHostObject(controller);
  if (object == nullptr) {
    return E_OUTOFMEMORY;
  }
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
