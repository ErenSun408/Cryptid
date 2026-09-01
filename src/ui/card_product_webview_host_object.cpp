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
#include <vector>

namespace usb_vault::ui {
namespace {

constexpr DISPID kSubmitLoginPasswordBytesId = 1;
constexpr DISPID kSubmitInitializationPasswordBytesId = 2;
constexpr DISPID kUnmountId = 3;
constexpr DISPID kHasActiveSessionId = 4;
constexpr DISPID kGetSessionStateId = 5;
constexpr DISPID kCreateContainerMibId = 10;
constexpr DISPID kMountContainerId = 11;
constexpr DISPID kMountAdoptedContainerId = 12;
constexpr DISPID kUnmountContainerId = 13;
constexpr DISPID kGetContainerCountId = 14;
constexpr DISPID kGetContainerIdId = 15;
constexpr DISPID kGetContainerNameId = 16;
constexpr DISPID kGetContainerMountedId = 17;
constexpr DISPID kGetContainerSizeMibId = 18;
constexpr DISPID kGetContainerDriveLetterId = 19;
constexpr DISPID kCreateContainerBMibId = 20;
constexpr wchar_t kSubmitLoginPasswordBytesName[] =
    L"submitLoginPasswordBytes";
constexpr wchar_t kSubmitInitializationPasswordBytesName[] =
    L"submitInitializationPasswordBytes";
constexpr wchar_t kUnmountName[] = L"unmount";
constexpr wchar_t kHasActiveSessionName[] = L"hasActiveSession";
constexpr wchar_t kGetSessionStateName[] = L"getSessionState";
constexpr wchar_t kCreateContainerMibName[] = L"createContainerMib";
constexpr wchar_t kMountContainerName[] = L"mountContainer";
constexpr wchar_t kMountAdoptedContainerName[] = L"mountAdoptedContainer";
constexpr wchar_t kUnmountContainerName[] = L"unmountContainer";
constexpr wchar_t kGetContainerCountName[] = L"getContainerCount";
constexpr wchar_t kGetContainerIdName[] = L"getContainerId";
constexpr wchar_t kGetContainerNameName[] = L"getContainerName";
constexpr wchar_t kGetContainerMountedName[] = L"getContainerMounted";
constexpr wchar_t kGetContainerSizeMibName[] = L"getContainerSizeMib";
constexpr wchar_t kGetContainerDriveLetterName[] =
    L"getContainerDriveLetter";
constexpr wchar_t kCreateContainerBMibName[] = L"createContainerBMib";

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
    if (matches(names[0], kCreateContainerMibName)) {
      ids[0] = kCreateContainerMibId;
      return S_OK;
    }
    if (matches(names[0], kMountContainerName)) {
      ids[0] = kMountContainerId;
      return S_OK;
    }
    if (matches(names[0], kMountAdoptedContainerName)) {
      ids[0] = kMountAdoptedContainerId;
      return S_OK;
    }
    if (matches(names[0], kUnmountContainerName)) {
      ids[0] = kUnmountContainerId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerCountName)) {
      ids[0] = kGetContainerCountId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerIdName)) {
      ids[0] = kGetContainerIdId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerNameName)) {
      ids[0] = kGetContainerNameId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerMountedName)) {
      ids[0] = kGetContainerMountedId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerSizeMibName)) {
      ids[0] = kGetContainerSizeMibId;
      return S_OK;
    }
    if (matches(names[0], kGetContainerDriveLetterName)) {
      ids[0] = kGetContainerDriveLetterId;
      return S_OK;
    }
    if (matches(names[0], kCreateContainerBMibName)) {
      ids[0] = kCreateContainerBMibId;
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
      case kCreateContainerMibId:
        return invoke_create_container(parameters, result, argument_error);
      case kCreateContainerBMibId:
        return invoke_create_container_b(parameters, argument_error);
      case kMountContainerId:
        return invoke_mount_container(parameters, result);
      case kMountAdoptedContainerId:
        return invoke_mount_adopted_container(parameters, result,
                                              argument_error);
      case kUnmountContainerId:
        return invoke_unmount_container(parameters, argument_error);
      case kGetContainerCountId:
        return invoke_container_count(parameters, result);
      case kGetContainerIdId:
      case kGetContainerNameId:
      case kGetContainerMountedId:
      case kGetContainerSizeMibId:
      case kGetContainerDriveLetterId:
        return invoke_container_fact(member, parameters, result,
                                     argument_error);
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

  [[nodiscard]] static bool exact_uint32(
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

  HRESULT invoke_create_container(DISPPARAMS* const parameters,
                                  VARIANT* const result,
                                  UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t size_mib = 0;
    if (!exact_uint32(parameters->rgvarg[0], size_mib)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      return complete_selection(controller_.create_container(size_mib),
                                result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Five arguments: outer capacity, hidden capacity, the password the caller
  // is logged in under, and the new second password twice. COM delivers them
  // in reverse order, and every supplied array is wiped on every path.
  HRESULT invoke_create_container_b(
      DISPPARAMS* const parameters, UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 5 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      wipe_available_arguments(parameters, 5);
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t outer_size_mib = 0;
    std::uint32_t hidden_size_mib = 0;
    if (!exact_uint32(parameters->rgvarg[4], outer_size_mib) ||
        !exact_uint32(parameters->rgvarg[3], hidden_size_mib)) {
      wipe_available_arguments(parameters, 5);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }

    VARIANT& login_argument = parameters->rgvarg[2];
    VARIANT& second_argument = parameters->rgvarg[1];
    VARIANT& confirmation_argument = parameters->rgvarg[0];
    security::SecureBuffer login;
    security::SecureBuffer second;
    security::SecureBuffer confirmation;
    try {
      login = consume_webview_password_array(login_argument);
      second = consume_webview_password_array(second_argument);
      confirmation = consume_webview_password_array(confirmation_argument);
    } catch (const std::invalid_argument&) {
      wipe_available_arguments(parameters, 5);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      wipe_available_arguments(parameters, 5);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      wipe_available_arguments(parameters, 5);
      return E_FAIL;
    }

    try {
      controller_.create_container_b(outer_size_mib, hidden_size_mib,
                                     login.bytes(), second.bytes(),
                                     confirmation.bytes());
      return S_OK;
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_mount_container(DISPPARAMS* const parameters,
                                 VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      return complete_selection(controller_.mount_container(), result);
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_mount_adopted_container(
      DISPPARAMS* const parameters, VARIANT* const result,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t identifier = 0;
    if (!exact_uint32(parameters->rgvarg[0], identifier)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.mount_container(identifier);
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_unmount_container(DISPPARAMS* const parameters,
                                   UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t identifier = 0;
    if (!exact_uint32(parameters->rgvarg[0], identifier)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.unmount_container(identifier);
      return S_OK;
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_container_count(DISPPARAMS* const parameters,
                                 VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (result == nullptr) return S_OK;
    try {
      const std::vector<ContainerUiStatus> statuses =
          controller_.container_statuses();
      ::VariantInit(result);
      result->vt = VT_UI4;
      result->ulVal = static_cast<ULONG>(statuses.size());
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  // One live fact of one adopted container. The page reads the count first and
  // then each fact by position, so an index that no longer exists is a raced
  // read and fails instead of returning a substitute value.
  HRESULT invoke_container_fact(const DISPID member,
                                DISPPARAMS* const parameters,
                                VARIANT* const result,
                                UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    std::uint32_t index = 0;
    if (!exact_uint32(parameters->rgvarg[0], index)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    if (result == nullptr) return S_OK;
    try {
      const std::vector<ContainerUiStatus> statuses =
          controller_.container_statuses();
      if (index >= statuses.size()) return E_INVALIDARG;
      const ContainerUiStatus& status = statuses[index];
      ::VariantInit(result);
      switch (member) {
        case kGetContainerIdId:
          result->vt = VT_UI4;
          result->ulVal = status.id;
          return S_OK;
        case kGetContainerMountedId:
          result->vt = VT_BOOL;
          result->boolVal = status.mounted ? VARIANT_TRUE : VARIANT_FALSE;
          return S_OK;
        case kGetContainerSizeMibId:
          result->vt = VT_UI4;
          result->ulVal = status.size_mib;
          return S_OK;
        case kGetContainerNameId:
          return complete_string(status.name.c_str(),
                                 static_cast<UINT>(status.name.size()),
                                 result);
        case kGetContainerDriveLetterId:
        default: {
          const wchar_t drive[] = {status.drive_letter, L':'};
          return complete_string(
              drive, status.drive_letter == wchar_t{} ? 0U : 2U, result);
        }
      }
    } catch (...) {
      return E_FAIL;
    }
  }

  [[nodiscard]] static HRESULT complete_selection(
      const bool selected, VARIANT* const result) noexcept {
    if (result != nullptr) {
      ::VariantInit(result);
      result->vt = VT_BOOL;
      result->boolVal = selected ? VARIANT_TRUE : VARIANT_FALSE;
    }
    return S_OK;
  }

  [[nodiscard]] static HRESULT complete_string(
      const wchar_t* const value, const UINT length,
      VARIANT* const result) noexcept {
    BSTR allocated = ::SysAllocStringLen(value, length);
    if (allocated == nullptr) return E_OUTOFMEMORY;
    result->vt = VT_BSTR;
    result->bstrVal = allocated;
    return S_OK;
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
