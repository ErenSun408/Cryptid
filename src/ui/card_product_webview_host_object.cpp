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
#include <string>
#include <string_view>
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
constexpr DISPID kGetOperationCountId = 21;
constexpr DISPID kGetOperationCompletedAtId = 22;
constexpr DISPID kGetOperationKindId = 23;
constexpr DISPID kGetOperationOutcomeId = 24;
constexpr DISPID kGetOperationContainerNameId = 25;
constexpr DISPID kGetOperationDriveLetterId = 26;
constexpr DISPID kClearOperationHistoryId = 27;
constexpr wchar_t kSubmitLoginPasswordBytesName[] = L"submitLoginPasswordBytes";
constexpr wchar_t kSubmitInitializationPasswordBytesName[] =
    L"submitInitializationPasswordBytes";
constexpr wchar_t kUnmountName[] = L"unmount";
constexpr wchar_t kHasActiveSessionName[] = L"hasActiveSession";
constexpr wchar_t kGetSessionStateName[] = L"getSessionState";
constexpr wchar_t kCreateContainerMibName[] = L"createContainerMib";
constexpr wchar_t kMountContainerName[] = L"mountContainer";
constexpr wchar_t kMountAdoptedContainerName[] = L"mountAdoptedContainer";
constexpr wchar_t kUnmountContainerName[] = L"unmountContainer";
constexpr wchar_t kForgetContainerName[] = L"forgetContainer";
constexpr wchar_t kGetRememberContainerPathsName[] =
    L"getRememberContainerPaths";
constexpr wchar_t kSetRememberContainerPathsName[] =
    L"setRememberContainerPaths";
constexpr wchar_t kGetMountImportedContainersName[] =
    L"getMountImportedContainers";
constexpr wchar_t kSetMountImportedContainersName[] =
    L"setMountImportedContainers";
constexpr wchar_t kGetContainerCountName[] = L"getContainerCount";
constexpr wchar_t kGetContainerIdName[] = L"getContainerId";
constexpr wchar_t kGetContainerNameName[] = L"getContainerName";
constexpr wchar_t kGetContainerMountedName[] = L"getContainerMounted";
constexpr wchar_t kGetContainerSizeMibName[] = L"getContainerSizeMib";
constexpr wchar_t kGetContainerDriveLetterName[] =
    L"getContainerDriveLetter";
constexpr wchar_t kCreateContainerBMibName[] = L"createContainerBMib";
constexpr wchar_t kGetOperationCountName[] = L"getOperationCount";
constexpr wchar_t kGetOperationCompletedAtName[] =
    L"getOperationCompletedAtUnixMs";
constexpr wchar_t kGetOperationKindName[] = L"getOperationKind";
constexpr wchar_t kGetOperationOutcomeName[] = L"getOperationOutcome";
constexpr wchar_t kGetOperationContainerNameName[] =
    L"getOperationContainerName";
constexpr wchar_t kGetOperationDriveLetterName[] = L"getOperationDriveLetter";
constexpr wchar_t kClearOperationHistoryName[] = L"clearOperationHistory";
constexpr DISPID kResetCardRecordId = 28;
constexpr wchar_t kResetCardRecordName[] = L"resetCardRecord";
constexpr DISPID kCanResetCardRecordId = 29;
constexpr wchar_t kCanResetCardRecordName[] = L"canResetCardRecord";
constexpr DISPID kChooseContainerDirectoryId = 30;
constexpr DISPID kForgetContainerId = 31;
constexpr DISPID kGetRememberContainerPathsId = 32;
constexpr DISPID kSetRememberContainerPathsId = 33;
constexpr DISPID kGetMountImportedContainersId = 34;
constexpr DISPID kSetMountImportedContainersId = 35;

constexpr DISPID kListContainerDirectoryId = 36;
constexpr DISPID kDeleteContainerEntryId = 37;
constexpr DISPID kRenameContainerEntryId = 38;
constexpr DISPID kCreateContainerDirectoryId = 39;
constexpr DISPID kMoveContainerEntryId = 40;
constexpr DISPID kCopyContainerEntryId = 41;

constexpr wchar_t kListContainerDirectoryName[] = L"listContainerDirectory";
constexpr wchar_t kDeleteContainerEntryName[] = L"deleteContainerEntry";
constexpr wchar_t kRenameContainerEntryName[] = L"renameContainerEntry";
constexpr wchar_t kCreateContainerDirectoryName[] = L"createContainerDirectory";
constexpr wchar_t kMoveContainerEntryName[] = L"moveContainerEntry";
constexpr wchar_t kCopyContainerEntryName[] = L"copyContainerEntry";

constexpr wchar_t kChooseContainerDirectoryName[] =
    L"chooseContainerDirectory";

// 🔴 A whole directory comes back as one packed string rather than through
// per-entry accessors like the container list above. Five hundred files under
// that pattern would be two thousand serialized asynchronous calls, and the
// page would visibly stall; this is one.
//
// Records are separated by one control character and the fields inside a
// record by another. Neither can occur in a name that came off a mounted
// volume -- Windows refuses control characters in a file name, and the
// controller refuses a listing carrying one before it reaches here -- so the
// format needs no escaping, and therefore has no escaping to get wrong.
//
// The first record is a header holding only the truncation flag, so an empty
// directory arrives as one record rather than as an empty string, which would
// be indistinguishable from a call that returned nothing.
constexpr wchar_t kListingRecordSeparator = L'\x1e';
constexpr wchar_t kListingFieldSeparator = L'\x1f';

[[nodiscard]] std::wstring pack_directory_listing(
    const DirectoryUiListing& listing) {
  std::wstring packed;
  packed.push_back(listing.truncated ? L'1' : L'0');
  for (const DirectoryEntryUiStatus& entry : listing.entries) {
    packed.push_back(kListingRecordSeparator);
    packed.append(entry.name);
    packed.push_back(kListingFieldSeparator);
    packed.push_back(entry.is_directory ? L'1' : L'0');
    packed.push_back(kListingFieldSeparator);
    packed.push_back(entry.is_link ? L'1' : L'0');
    packed.push_back(kListingFieldSeparator);
    packed.append(std::to_wstring(entry.size_bytes));
    packed.push_back(kListingFieldSeparator);
    packed.append(std::to_wstring(entry.modified_unix_ms));
  }
  return packed;
}

// A container name and a save location are not secrets the way a password is,
// but a container path is precisely what section 8.4 keeps off this machine,
// so the marshalled copy does not outlive the call that carried it. The BSTR
// stays valid and keeps its length; only its characters go. The page's own
// copies are cleared on the page side, where the DOM holds them.
class LocationStringWiper final {
 public:
  explicit LocationStringWiper(std::wstring& value) noexcept
      : value_(value) {}
  ~LocationStringWiper() noexcept {
    if (!value_.empty()) {
      static_cast<void>(
          ::SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t)));
    }
  }

  LocationStringWiper(const LocationStringWiper&) = delete;
  LocationStringWiper& operator=(const LocationStringWiper&) = delete;

 private:
  std::wstring& value_;
};

class LocationArgumentsWiper final {
 public:
  LocationArgumentsWiper(VARIANT& first, VARIANT& second) noexcept
      : first_(first), second_(second) {}
  ~LocationArgumentsWiper() noexcept {
    wipe(first_);
    wipe(second_);
  }

  LocationArgumentsWiper(const LocationArgumentsWiper&) = delete;
  LocationArgumentsWiper& operator=(const LocationArgumentsWiper&) = delete;

 private:
  // Anything that is not a string was never interpreted as one, so it is left
  // exactly as it arrived.
  static void wipe(VARIANT& value) noexcept {
    if (value.vt != VT_BSTR || value.bstrVal == nullptr) return;
    const UINT length = ::SysStringLen(value.bstrVal);
    if (length == 0) return;
    static_cast<void>(
        ::SecureZeroMemory(value.bstrVal, length * sizeof(wchar_t)));
  }

  VARIANT& first_;
  VARIANT& second_;
};

// The directory a page is looking at and the name it picked are the contents
// of an encrypted volume, so the marshalled copies go the way a container
// location does. Every string argument of the call is cleared; anything that
// is not a string was never read as one and is left exactly as it arrived.
class DispatchTextWiper final {
 public:
  explicit DispatchTextWiper(DISPPARAMS* const parameters) noexcept
      : parameters_(parameters) {}

  ~DispatchTextWiper() noexcept {
    if (parameters_ == nullptr || parameters_->rgvarg == nullptr) return;
    for (UINT index = 0; index < parameters_->cArgs; ++index) {
      VARIANT& value = parameters_->rgvarg[index];
      if (value.vt != VT_BSTR || value.bstrVal == nullptr) continue;
      const UINT length = ::SysStringLen(value.bstrVal);
      if (length == 0) continue;
      static_cast<void>(
          ::SecureZeroMemory(value.bstrVal, length * sizeof(wchar_t)));
    }
  }

  DispatchTextWiper(const DispatchTextWiper&) = delete;
  DispatchTextWiper& operator=(const DispatchTextWiper&) = delete;

 private:
  DISPPARAMS* parameters_;
};

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

[[nodiscard]] LONG operation_kind_wire_value(
    const OperationUiKind kind) noexcept {
  switch (kind) {
    case OperationUiKind::session_opened:
      return 0;
    case OperationUiKind::container_created:
      return 1;
    case OperationUiKind::container_mounted:
      return 2;
    case OperationUiKind::container_unmounted:
    default:
      return 3;
  }
}

[[nodiscard]] LONG operation_outcome_wire_value(
    const OperationUiOutcome outcome) noexcept {
  return outcome == OperationUiOutcome::succeeded ? 0 : 1;
}

class CardProductWebViewHostObject final : public IDispatch {
 public:
  CardProductWebViewHostObject(
      CardProductUiSessionController& controller,
      IUiCloseRequestSink* const close_sink) noexcept
      : controller_(controller), close_sink_(close_sink) {}

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
    if (matches(names[0], kChooseContainerDirectoryName)) {
      ids[0] = kChooseContainerDirectoryId;
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
    if (matches(names[0], kForgetContainerName)) {
      ids[0] = kForgetContainerId;
      return S_OK;
    }
    if (matches(names[0], kGetRememberContainerPathsName)) {
      ids[0] = kGetRememberContainerPathsId;
      return S_OK;
    }
    if (matches(names[0], kSetRememberContainerPathsName)) {
      ids[0] = kSetRememberContainerPathsId;
      return S_OK;
    }
    if (matches(names[0], kGetMountImportedContainersName)) {
      ids[0] = kGetMountImportedContainersId;
      return S_OK;
    }
    if (matches(names[0], kSetMountImportedContainersName)) {
      ids[0] = kSetMountImportedContainersId;
      return S_OK;
    }
    if (matches(names[0], kListContainerDirectoryName)) {
      ids[0] = kListContainerDirectoryId;
      return S_OK;
    }
    if (matches(names[0], kDeleteContainerEntryName)) {
      ids[0] = kDeleteContainerEntryId;
      return S_OK;
    }
    if (matches(names[0], kRenameContainerEntryName)) {
      ids[0] = kRenameContainerEntryId;
      return S_OK;
    }
    if (matches(names[0], kCreateContainerDirectoryName)) {
      ids[0] = kCreateContainerDirectoryId;
      return S_OK;
    }
    if (matches(names[0], kMoveContainerEntryName)) {
      ids[0] = kMoveContainerEntryId;
      return S_OK;
    }
    if (matches(names[0], kCopyContainerEntryName)) {
      ids[0] = kCopyContainerEntryId;
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
    if (matches(names[0], kGetOperationCountName)) {
      ids[0] = kGetOperationCountId;
      return S_OK;
    }
    if (matches(names[0], kGetOperationCompletedAtName)) {
      ids[0] = kGetOperationCompletedAtId;
      return S_OK;
    }
    if (matches(names[0], kGetOperationKindName)) {
      ids[0] = kGetOperationKindId;
      return S_OK;
    }
    if (matches(names[0], kGetOperationOutcomeName)) {
      ids[0] = kGetOperationOutcomeId;
      return S_OK;
    }
    if (matches(names[0], kGetOperationContainerNameName)) {
      ids[0] = kGetOperationContainerNameId;
      return S_OK;
    }
    if (matches(names[0], kGetOperationDriveLetterName)) {
      ids[0] = kGetOperationDriveLetterId;
      return S_OK;
    }
    if (matches(names[0], kClearOperationHistoryName)) {
      ids[0] = kClearOperationHistoryId;
      return S_OK;
    }
    if (matches(names[0], kResetCardRecordName)) {
      ids[0] = kResetCardRecordId;
      return S_OK;
    }
    if (matches(names[0], kCanResetCardRecordName)) {
      ids[0] = kCanResetCardRecordId;
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
      case kChooseContainerDirectoryId:
        return invoke_choose_container_directory(parameters, result);
      case kCreateContainerMibId:
        return invoke_create_container(parameters, result, argument_error);
      case kCreateContainerBMibId:
        return invoke_create_container_b(parameters, result, argument_error);
      case kMountContainerId:
        return invoke_mount_container(parameters, result);
      case kMountAdoptedContainerId:
        return invoke_mount_adopted_container(parameters, result,
                                              argument_error);
      case kUnmountContainerId:
        return invoke_unmount_container(parameters, argument_error);
      case kForgetContainerId:
        return invoke_forget_container(parameters, argument_error);
      case kGetRememberContainerPathsId:
        return invoke_get_remember_container_paths(parameters, result);
      case kSetRememberContainerPathsId:
        return invoke_set_remember_container_paths(parameters, argument_error);
      case kGetMountImportedContainersId:
        return invoke_get_mount_imported_containers(parameters, result);
      case kSetMountImportedContainersId:
        return invoke_set_mount_imported_containers(parameters, argument_error);
      case kListContainerDirectoryId:
        return invoke_list_container_directory(parameters, result,
                                               argument_error);
      case kDeleteContainerEntryId:
      case kCreateContainerDirectoryId:
        return invoke_entry_by_name(member, parameters, result, argument_error);
      case kRenameContainerEntryId:
        return invoke_rename_container_entry(parameters, result,
                                             argument_error);
      case kMoveContainerEntryId:
      case kCopyContainerEntryId:
        return invoke_transfer_container_entry(member, parameters, result,
                                               argument_error);
      case kGetContainerCountId:
        return invoke_container_count(parameters, result);
      case kGetContainerIdId:
      case kGetContainerNameId:
      case kGetContainerMountedId:
      case kGetContainerSizeMibId:
      case kGetContainerDriveLetterId:
        return invoke_container_fact(
            member, parameters, result, argument_error);
      case kGetOperationCountId:
        return invoke_operation_count(parameters, result);
      case kGetOperationCompletedAtId:
      case kGetOperationKindId:
      case kGetOperationOutcomeId:
      case kGetOperationContainerNameId:
      case kGetOperationDriveLetterId:
        return invoke_operation_fact(
            member, parameters, result, argument_error);
      case kClearOperationHistoryId:
        return invoke_clear_operation_history(parameters);
      case kResetCardRecordId:
        return invoke_reset_card_record(parameters);
      case kCanResetCardRecordId:
        return invoke_can_reset_card_record(parameters, result);
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

  // The browse button. Returns the directory the user picked, or an empty
  // string when they cancelled, and decides nothing: the value goes back into
  // the form field and is checked again when the form is submitted.
  HRESULT invoke_choose_container_directory(
      DISPPARAMS* const parameters, VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (result == nullptr) return S_OK;
    std::wstring directory;
    try {
      directory = controller_.browse_for_container_directory();
    } catch (...) {
      return E_FAIL;
    }
    LocationStringWiper wiper(directory);
    ::VariantInit(result);
    return complete_string(directory.c_str(),
                           static_cast<UINT>(directory.size()), result);
  }

  // Three arguments: the container name, the save location, and the capacity.
  // The first two are the page-supplied strings this boundary did not carry
  // before 2026-09-05; they are wiped in the caller's own buffers on every
  // path, and the controller decides whether they name a location at all.
  // COM delivers the arguments in reverse order.
  // A page-supplied string. An absent or empty BSTR reads as an empty view,
  // which every caller rejects downstream rather than treating as a default.
  [[nodiscard]] static bool exact_wstring(
      const VARIANTARG& argument, std::wstring_view& result) noexcept {
    if (argument.vt != VT_BSTR) return false;
    if (argument.bstrVal == nullptr) {
      result = {};
      return true;
    }
    result = std::wstring_view(argument.bstrVal,
                               ::SysStringLen(argument.bstrVal));
    return true;
  }

  HRESULT invoke_create_container(DISPPARAMS* const parameters,
                                  VARIANT* const result,
                                  UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 3 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    LocationArgumentsWiper location_wiper(parameters->rgvarg[2],
                                          parameters->rgvarg[1]);
    std::wstring_view name;
    std::wstring_view directory;
    std::uint32_t size_mib = 0;
    if (!exact_wstring(parameters->rgvarg[2], name) ||
        !exact_wstring(parameters->rgvarg[1], directory) ||
        !exact_uint32(parameters->rgvarg[0], size_mib)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.create_container(name, directory, size_mib);
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Six arguments: the container name, the save location, outer capacity,
  // hidden capacity, and the new second password twice. The password the
  // session was opened under is still not one of them: the session holds it,
  // so it never returns to the page. The location is now among them, and the
  // argument list stays word-for-word parallel to the standard container's for
  // the same reason the two dialogs do (8.7.2). COM delivers the arguments in
  // reverse order; every supplied array and both strings are wiped on every
  // path.
  HRESULT invoke_create_container_b(
      DISPPARAMS* const parameters, VARIANT* const result,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 6 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      wipe_available_arguments(parameters, 6);
      return DISP_E_BADPARAMCOUNT;
    }
    LocationArgumentsWiper location_wiper(parameters->rgvarg[5],
                                          parameters->rgvarg[4]);
    std::wstring_view name;
    std::wstring_view directory;
    std::uint32_t outer_size_mib = 0;
    std::uint32_t hidden_size_mib = 0;
    if (!exact_wstring(parameters->rgvarg[5], name) ||
        !exact_wstring(parameters->rgvarg[4], directory) ||
        !exact_uint32(parameters->rgvarg[3], outer_size_mib) ||
        !exact_uint32(parameters->rgvarg[2], hidden_size_mib)) {
      wipe_available_arguments(parameters, 6);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }

    VARIANT& second_argument = parameters->rgvarg[1];
    VARIANT& confirmation_argument = parameters->rgvarg[0];
    security::SecureBuffer second;
    security::SecureBuffer confirmation;
    try {
      second = consume_webview_password_array(second_argument);
      confirmation = consume_webview_password_array(confirmation_argument);
    } catch (const std::invalid_argument&) {
      wipe_available_arguments(parameters, 6);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      wipe_available_arguments(parameters, 6);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      wipe_available_arguments(parameters, 6);
      return E_FAIL;
    }

    try {
      controller_.create_container_b(name, directory, outer_size_mib,
                                     hidden_size_mib, second.bytes(),
                                     confirmation.bytes());
      return complete_selection(true, result);
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

  HRESULT invoke_get_remember_container_paths(
      DISPPARAMS* const parameters, VARIANT* const result) noexcept {
    if (parameters == nullptr || parameters->cArgs != 0 ||
        parameters->cNamedArgs != 0) {
      return DISP_E_BADPARAMCOUNT;
    }
    try {
      const bool remember = controller_.remembering_container_paths();
      if (result != nullptr) {
        ::VariantInit(result);
        result->vt = VT_BOOL;
        result->boolVal = remember ? VARIANT_TRUE : VARIANT_FALSE;
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_set_remember_container_paths(
      DISPPARAMS* const parameters, UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const VARIANT& argument = parameters->rgvarg[0];
    if (argument.vt != VT_BOOL) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.set_remembering_container_paths(argument.boolVal !=
                                                  VARIANT_FALSE);
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_get_mount_imported_containers(
      DISPPARAMS* const parameters, VARIANT* const result) noexcept {
    if (parameters == nullptr || parameters->cArgs != 0 ||
        parameters->cNamedArgs != 0) {
      return DISP_E_BADPARAMCOUNT;
    }
    try {
      const bool mount = controller_.mounting_imported_containers();
      if (result != nullptr) {
        ::VariantInit(result);
        result->vt = VT_BOOL;
        result->boolVal = mount ? VARIANT_TRUE : VARIANT_FALSE;
      }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_set_mount_imported_containers(
      DISPPARAMS* const parameters, UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const VARIANT& argument = parameters->rgvarg[0];
    if (argument.vt != VT_BOOL) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.set_mounting_imported_containers(argument.boolVal !=
                                                   VARIANT_FALSE);
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Two arguments: the container identifier and the directory inside it. COM
  // delivers them in reverse order, as it does everywhere here.
  HRESULT invoke_list_container_directory(
      DISPPARAMS* const parameters, VARIANT* const result,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 2 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t id = 0;
    std::wstring_view relative_directory;
    if (!exact_uint32(parameters->rgvarg[1], id) ||
        !exact_wstring(parameters->rgvarg[0], relative_directory)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    if (result == nullptr) return S_OK;

    std::wstring packed;
    try {
      packed = pack_directory_listing(
          controller_.list_container_directory(id, relative_directory));
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
    // The packed listing is a volume's file names, so it is cleared here for
    // the same reason a container location is.
    LocationStringWiper packed_wiper(packed);
    ::VariantInit(result);
    return complete_string(packed.c_str(), static_cast<UINT>(packed.size()),
                           result);
  }

  // Three arguments: the container, the directory the page is looking at, and
  // the name it picked. Deleting and creating a directory take the same three
  // and differ only in what they then do with them.
  HRESULT invoke_entry_by_name(const DISPID member,
                               DISPPARAMS* const parameters,
                               VARIANT* const result,
                               UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 3 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t id = 0;
    std::wstring_view relative_directory;
    std::wstring_view name;
    if (!exact_uint32(parameters->rgvarg[2], id) ||
        !exact_wstring(parameters->rgvarg[1], relative_directory) ||
        !exact_wstring(parameters->rgvarg[0], name)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      if (member == kDeleteContainerEntryId) {
        controller_.delete_container_entry(id, relative_directory, name);
      } else {
        controller_.create_container_directory(id, relative_directory, name);
      }
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Four arguments: the container, the directory, the current name, and the
  // new one.
  HRESULT invoke_rename_container_entry(
      DISPPARAMS* const parameters, VARIANT* const result,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 4 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t id = 0;
    std::wstring_view relative_directory;
    std::wstring_view name;
    std::wstring_view new_name;
    if (!exact_uint32(parameters->rgvarg[3], id) ||
        !exact_wstring(parameters->rgvarg[2], relative_directory) ||
        !exact_wstring(parameters->rgvarg[1], name) ||
        !exact_wstring(parameters->rgvarg[0], new_name)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.rename_container_entry(id, relative_directory, name,
                                         new_name);
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Five arguments: where the entry is -- container, directory, name -- and
  // where it goes -- container and directory. The name is not repeated,
  // because neither of these renames anything.
  HRESULT invoke_transfer_container_entry(
      const DISPID member, DISPPARAMS* const parameters,
      VARIANT* const result, UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 5 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t source_id = 0;
    std::wstring_view source_relative_directory;
    std::wstring_view name;
    std::uint32_t destination_id = 0;
    std::wstring_view destination_relative_directory;
    if (!exact_uint32(parameters->rgvarg[4], source_id) ||
        !exact_wstring(parameters->rgvarg[3], source_relative_directory) ||
        !exact_wstring(parameters->rgvarg[2], name) ||
        !exact_uint32(parameters->rgvarg[1], destination_id) ||
        !exact_wstring(parameters->rgvarg[0],
                       destination_relative_directory)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      if (member == kMoveContainerEntryId) {
        controller_.move_container_entry(source_id, source_relative_directory,
                                         name, destination_id,
                                         destination_relative_directory);
      } else {
        controller_.copy_container_entry(source_id, source_relative_directory,
                                         name, destination_id,
                                         destination_relative_directory);
      }
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_forget_container(DISPPARAMS* const parameters,
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
      controller_.forget_container(identifier);
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

  HRESULT invoke_operation_count(DISPPARAMS* const parameters,
                                 VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (result == nullptr) return S_OK;
    try {
      const std::vector<OperationUiEvent> events =
          controller_.operation_history();
      ::VariantInit(result);
      result->vt = VT_UI4;
      result->ulVal = static_cast<ULONG>(events.size());
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_operation_fact(const DISPID member,
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
      const std::vector<OperationUiEvent> events =
          controller_.operation_history();
      if (index >= events.size()) return E_INVALIDARG;
      const OperationUiEvent& event = events[index];
      ::VariantInit(result);
      switch (member) {
        case kGetOperationCompletedAtId:
          result->vt = VT_R8;
          result->dblVal = static_cast<double>(event.completed_at_unix_ms);
          return S_OK;
        case kGetOperationKindId:
          result->vt = VT_I4;
          result->lVal = operation_kind_wire_value(event.kind);
          return S_OK;
        case kGetOperationOutcomeId:
          result->vt = VT_I4;
          result->lVal = operation_outcome_wire_value(event.outcome);
          return S_OK;
        case kGetOperationContainerNameId:
          return complete_string(event.container_name.c_str(),
                                 static_cast<UINT>(event.container_name.size()),
                                 result);
        case kGetOperationDriveLetterId:
        default: {
          const wchar_t drive[] = {event.drive_letter, L':'};
          return complete_string(
              drive, event.drive_letter == wchar_t{} ? 0U : 2U, result);
        }
      }
    } catch (...) {
      return E_FAIL;
    }
  }

  HRESULT invoke_can_reset_card_record(DISPPARAMS* const parameters,
                                      VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    bool available = false;
    try {
      available = controller_.can_reset_card_record();
    } catch (...) {
      return E_FAIL;
    }
    return complete_selection(available, result);
  }

  HRESULT invoke_reset_card_record(DISPPARAMS* const parameters) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      controller_.reset_card_record();
    } catch (...) {
      return E_FAIL;
    }
    // Only a committed reset asks for the close, and it asks after the card
    // and the session are already consistent.
    if (close_sink_ != nullptr) close_sink_->request_close();
    return S_OK;
  }

  HRESULT
  invoke_clear_operation_history(DISPPARAMS* const parameters) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      controller_.clear_operation_history();
      return S_OK;
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
  IUiCloseRequestSink* close_sink_{};
};

}  // namespace

HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* const close_sink,
    IDispatch** const result) noexcept {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  auto* const object =
      new (std::nothrow) CardProductWebViewHostObject(controller, close_sink);
  if (object == nullptr) return E_OUTOFMEMORY;
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
