#include "usb_vault/ui/card_product_webview_host_object.h"

#include "usb_vault/ui/webview_password_array.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "usb_vault/volume/volume_provider.h"

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
#include <thread>
#include <mutex>

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
constexpr DISPID kExportContainerEntryId = 42;
constexpr DISPID kExportContainerFileId = 43;
constexpr DISPID kSetWindowSizeId = 44;
constexpr DISPID kBeginCreateId = 47;
constexpr DISPID kBeginCreateBId = 48;
constexpr DISPID kCreationProgressId = 49;
constexpr DISPID kCancelCreationId = 50;
constexpr DISPID kBeginMountAdoptedId = 51;
constexpr DISPID kBeginUnmountContainerId = 52;
constexpr DISPID kBeginUnmountId = 53;
constexpr DISPID kVolumeOperationStatusId = 54;
constexpr DISPID kBeginMountBatchId = 57;
constexpr DISPID kBeginUnmountBatchId = 58;
constexpr DISPID kBatchProgressId = 59;
constexpr DISPID kBatchItemIdId = 60;
constexpr DISPID kBatchItemNameId = 61;
constexpr DISPID kBatchItemOutcomeId = 62;
constexpr DISPID kBeginForceUnmountBatchId = 63;
constexpr DISPID kGetForcePreferenceId = 64;
constexpr DISPID kSetForcePreferenceId = 65;
constexpr DISPID kGetWindowSizeId = 66;
constexpr DISPID kReorderContainersId = 67;
constexpr DISPID kGetAutoMountId = 68;
constexpr DISPID kSetAutoMountId = 69;
constexpr std::size_t kMaximumBatchItems = 1024;
bool is_batch_query(DISPID member) noexcept {
  return member >= kBatchProgressId && member <= kBatchItemOutcomeId;
}
constexpr DISPID kBeginForceUnmountLastId = 55;
constexpr DISPID kBeginMountAllId = 56;
// Public, role-neutral outcomes: idle/running/success/unknown, busy, missing,
// denied, unreadable container, IO failure, driver unavailable, card removed.
unsigned int volume_failure_status(const std::error_code& error) noexcept {
  using volume::DriverError;
  if (error == volume::make_error_code(DriverError::volume_busy) ||
      error == volume::make_error_code(DriverError::volume_locked)) return 4;
  // Windows maps sharing violations to generic permission_denied. Preserve
  // the more precise native reason before comparing generic conditions.
  if (error.category() == std::system_category()) {
    switch (error.value()) {
      case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return 4;
      case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return 5;
      case ERROR_ACCESS_DENIED: return 6;
      case ERROR_READ_FAULT: case ERROR_WRITE_FAULT: case ERROR_IO_DEVICE: return 8;
    }
  }
  if (error == std::errc::no_such_file_or_directory) return 5;
  if (error == std::errc::permission_denied) return 6;
  if (error == volume::make_error_code(DriverError::authentication_failed) ||
      error == volume::make_error_code(DriverError::damaged_volume)) return 7;
  if (error == volume::make_error_code(DriverError::read_failed) ||
      error == volume::make_error_code(DriverError::write_failed) ||
      error == volume::make_error_code(DriverError::seek_failed) ||
      error == std::errc::io_error) return 8;
  if (error == volume::make_error_code(DriverError::driver_or_drive_not_found) ||
      error == volume::make_error_code(DriverError::version_mismatch)) return 9;
  return 3;
}
constexpr DISPID kGetOperationLoggingEnabledId = 45;
constexpr DISPID kSetOperationLoggingEnabledId = 46;

constexpr wchar_t kListContainerDirectoryName[] = L"listContainerDirectory";
constexpr wchar_t kDeleteContainerEntryName[] = L"deleteContainerEntry";
constexpr wchar_t kRenameContainerEntryName[] = L"renameContainerEntry";
constexpr wchar_t kCreateContainerDirectoryName[] = L"createContainerDirectory";
constexpr wchar_t kMoveContainerEntryName[] = L"moveContainerEntry";
constexpr wchar_t kCopyContainerEntryName[] = L"copyContainerEntry";
constexpr wchar_t kExportContainerEntryName[] = L"exportContainerEntry";
constexpr wchar_t kExportContainerFileName[] = L"exportContainerFile";

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
      IUiCloseRequestSink* const close_sink,
      IUiWindowSizeSink* const window_size_sink,
      CreationWorkState* const creation_work) noexcept
      : controller_(controller), close_sink_(close_sink),
        window_size_sink_(window_size_sink),
        creation_work_(creation_work ? creation_work : &local_creation_work_) {}

  ~CardProductWebViewHostObject() {
    if (creation_thread_.joinable()) creation_thread_.join();
  }

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
    if (matches(names[0], L"setWindowSize")) {
      ids[0] = kSetWindowSizeId;
      return S_OK;
    }
    if (matches(names[0], L"getWindowSize")) { ids[0] = kGetWindowSizeId; return S_OK; }
    if (matches(names[0], L"getAutoMountEnabled")) { ids[0] = kGetAutoMountId; return S_OK; }
    if (matches(names[0], L"setAutoMountEnabled")) { ids[0] = kSetAutoMountId; return S_OK; }
    if (matches(names[0], L"reorderContainers")) { ids[0] = kReorderContainersId; return S_OK; }
    if (matches(names[0], L"getForceUnmountEnabled")) { ids[0] = kGetForcePreferenceId; return S_OK; }
    if (matches(names[0], L"setForceUnmountEnabled")) { ids[0] = kSetForcePreferenceId; return S_OK; }
    if (matches(names[0], L"beginCreateContainerMib")) { ids[0] = kBeginCreateId; return S_OK; }
    if (matches(names[0], L"beginCreateContainerBMib")) { ids[0] = kBeginCreateBId; return S_OK; }
    if (matches(names[0], L"getCreationProgress")) { ids[0] = kCreationProgressId; return S_OK; }
    if (matches(names[0], L"cancelContainerCreation")) { ids[0] = kCancelCreationId; return S_OK; }
    if (matches(names[0], L"getOperationLoggingEnabled")) {
      ids[0] = kGetOperationLoggingEnabledId;
      return S_OK;
    }
    if (matches(names[0], L"setOperationLoggingEnabled")) {
      ids[0] = kSetOperationLoggingEnabledId;
      return S_OK;
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
    if (matches(names[0], kExportContainerEntryName)) {
      ids[0] = kExportContainerEntryId;
      return S_OK;
    }
    if (matches(names[0], kExportContainerFileName)) {
      ids[0] = kExportContainerFileId;
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
    if (matches(names[0], L"beginMountAdoptedContainer")) { ids[0] = kBeginMountAdoptedId; return S_OK; }
    if (matches(names[0], L"beginMountAll")) { ids[0] = kBeginMountAllId; return S_OK; }
    if (matches(names[0], L"beginUnmountContainer")) { ids[0] = kBeginUnmountContainerId; return S_OK; }
    if (matches(names[0], L"beginUnmount")) { ids[0] = kBeginUnmountId; return S_OK; }
    if (matches(names[0], L"beginForceUnmountLast")) { ids[0] = kBeginForceUnmountLastId; return S_OK; }
    if (matches(names[0], L"getVolumeOperationStatus")) { ids[0] = kVolumeOperationStatusId; return S_OK; }
    if (matches(names[0], L"beginMountBatch")) { ids[0] = kBeginMountBatchId; return S_OK; }
    if (matches(names[0], L"beginUnmountBatch")) { ids[0] = kBeginUnmountBatchId; return S_OK; }
    if (matches(names[0], L"beginForceUnmountBatch")) { ids[0] = kBeginForceUnmountBatchId; return S_OK; }
    if (matches(names[0], L"getBatchProgress")) { ids[0] = kBatchProgressId; return S_OK; }
    if (matches(names[0], L"getBatchItemId")) { ids[0] = kBatchItemIdId; return S_OK; }
    if (matches(names[0], L"getBatchItemName")) { ids[0] = kBatchItemNameId; return S_OK; }
    if (matches(names[0], L"getBatchItemOutcome")) { ids[0] = kBatchItemOutcomeId; return S_OK; }
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
    // Workers exclusively own the session. Only their atomic progress is
    // readable; cancellation belongs to creation, never volume operations.
    if (creation_work_->running.load() &&
        (creation_work_->volume_operation.load()
             ? (batch_mode_ ? !is_batch_query(member) : member != kVolumeOperationStatusId)
             : member != kCreationProgressId && member != kCancelCreationId)) {
      DispatchTextWiper text_wiper(parameters);
      wipe_available_arguments(parameters, parameters ? parameters->cArgs : 0);
      return E_PENDING;
    }

    // Observations may refresh the failure dialog. Any unrelated mutation
    // revokes the last cleanup target, so a stale page cannot force a new mount.
    switch (member) {
      case kBeginForceUnmountLastId: case kVolumeOperationStatusId:
      case kBatchProgressId: case kBatchItemIdId: case kBatchItemNameId: case kBatchItemOutcomeId:
      case kHasActiveSessionId: case kGetSessionStateId:
      case kGetContainerCountId: case kGetContainerIdId: case kGetContainerNameId:
      case kGetContainerMountedId: case kGetContainerSizeMibId: case kGetContainerDriveLetterId:
      case kGetOperationCountId: case kGetOperationCompletedAtId: case kGetOperationKindId:
      case kGetOperationOutcomeId: case kGetOperationContainerNameId: case kGetOperationDriveLetterId:
      case kCanResetCardRecordId: case kGetRememberContainerPathsId:
      case kGetMountImportedContainersId: case kGetOperationLoggingEnabledId:
      case kGetForcePreferenceId: case kGetWindowSizeId:
      case kGetAutoMountId:
      case kSetForcePreferenceId: // Changing a preference must not discard retry results.
      case kListContainerDirectoryId: break;
      default: last_volume_member_ = 0; clear_batch(); break;
    }
    switch (member) {
      case kBeginMountBatchId: case kBeginUnmountBatchId:
      case kBeginForceUnmountBatchId:
        return begin_batch(member == kBeginMountBatchId, member == kBeginForceUnmountBatchId, parameters, result);
      case kReorderContainersId:
        return begin_batch(false, false, parameters, result, true);
      case kBatchProgressId: case kBatchItemIdId: case kBatchItemNameId: case kBatchItemOutcomeId:
        return invoke_batch_query(member, parameters, result);
      case kBeginMountAdoptedId:
      case kBeginMountAllId:
      case kBeginUnmountContainerId:
      case kBeginUnmountId:
      case kBeginForceUnmountLastId:
        return begin_volume_operation(member, parameters, result, argument_error);
      case kVolumeOperationStatusId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        ::VariantInit(result);
        result->vt = VT_UI4;
        result->ulVal = creation_work_->running.load() ? 1U : volume_status_.load();
        return S_OK;
      case kBeginCreateId: return invoke_create_container(parameters, result, argument_error, true);
      case kBeginCreateBId: return invoke_create_container_b(parameters, result, argument_error, true);
      case kCancelCreationId: {
        if (!parameters || parameters->cArgs != 0 || parameters->cNamedArgs != 0) return DISP_E_BADPARAMCOUNT;
        return complete_selection(creation_work_->request_cancel(), result);
      }
      case kCreationProgressId: {
        if (!parameters || parameters->cArgs != 0 || parameters->cNamedArgs != 0) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        try {
          const auto status = creation_work_->running.load() ? 1U : creation_status_.load();
          if (status != 1 && controller_.state() != CardProductUiState::authenticated) {
            creation_progress_.store(0);
            creation_flags_.store(0);
            creation_status_.store(0);
            return E_ACCESSDENIED;
          }
          const auto value = creation_progress_.load();
          const std::wstring packed = std::to_wstring(status) + L"," +
              std::to_wstring(value >> 32) + L"," + std::to_wstring((value >> 16) & 65535) + L"," + std::to_wstring(value & 65535) + L"," + std::to_wstring(creation_flags_.load());
          ::VariantInit(result); result->vt = VT_BSTR;
          result->bstrVal = ::SysAllocStringLen(packed.data(), static_cast<UINT>(packed.size()));
          return result->bstrVal ? S_OK : E_OUTOFMEMORY;
        } catch (...) { return E_FAIL; }
      }
      case kGetOperationLoggingEnabledId:
      case kGetAutoMountId:
      case kSetAutoMountId:
      case kGetForcePreferenceId:
      case kSetForcePreferenceId:
      case kSetOperationLoggingEnabledId: {
        const bool force = member == kGetForcePreferenceId || member == kSetForcePreferenceId;
        const bool automatic = member == kGetAutoMountId || member == kSetAutoMountId;
        const bool write = member == kSetOperationLoggingEnabledId || member == kSetForcePreferenceId || member == kSetAutoMountId;
        if (parameters == nullptr || parameters->cArgs != (write ? 1U : 0U) ||
            parameters->cNamedArgs != 0 || (write && parameters->rgvarg == nullptr)) {
          return DISP_E_BADPARAMCOUNT;
        }
        if (write && parameters->rgvarg[0].vt != VT_BOOL) {
          if (argument_error != nullptr) *argument_error = 0;
          return DISP_E_TYPEMISMATCH;
        }
        try {
          if (write) {
            if (force) last_volume_member_ = 0;
            if (force) controller_.set_force_unmount_enabled(parameters->rgvarg[0].boolVal != VARIANT_FALSE);
            else if (automatic) controller_.set_auto_mount_enabled(parameters->rgvarg[0].boolVal != VARIANT_FALSE);
            else controller_.set_operation_logging_enabled(parameters->rgvarg[0].boolVal != VARIANT_FALSE);
          } else {
            const bool enabled = force ? controller_.force_unmount_enabled() : automatic ? controller_.auto_mount_enabled() : controller_.operation_logging_enabled();
            if (result != nullptr) {
              ::VariantInit(result);
              result->vt = VT_BOOL;
              result->boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
            }
          }
          return S_OK;
        } catch (...) { return E_FAIL; }
      }
      case kSetWindowSizeId: {
        if (parameters == nullptr || parameters->cArgs != 1 ||
            parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
          return DISP_E_BADPARAMCOUNT;
        }
        std::uint32_t preset = 0;
        if (!exact_uint32(parameters->rgvarg[0], preset) || preset > 3) {
          if (argument_error != nullptr) *argument_error = 0;
          return DISP_E_TYPEMISMATCH;
        }
        return window_size_sink_ != nullptr &&
                       window_size_sink_->set_window_size(preset)
                   ? S_OK : E_FAIL;
      }
      case kGetWindowSizeId: {
        if (!parameters || parameters->cArgs != 0 || parameters->cNamedArgs != 0) return DISP_E_BADPARAMCOUNT;
        if (result) { ::VariantInit(result); result->vt = VT_UI4; result->ulVal = window_size_sink_ ? window_size_sink_->window_size() : 1U; }
        return S_OK;
      }
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
      case kExportContainerEntryId:
        return invoke_export_container_entry(parameters, result,
                                             argument_error);
      case kExportContainerFileId:
        return invoke_export_container_file(parameters, result,
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
  struct BatchItem {
    application::ContainerOperationTarget target;
    unsigned int outcome{};
  };
  void clear_batch() {
    std::lock_guard lock(batch_mutex_);
    batch_items_.clear();
    batch_mode_ = false;
  }

  // Ordering shares the bounded ID parser; it starts no volume worker.
  HRESULT begin_batch(bool mount, bool force, DISPPARAMS* parameters, VARIANT* result,
                      bool reorder = false) noexcept {
    if (!parameters || parameters->cArgs != 1 || parameters->cNamedArgs != 0 || !parameters->rgvarg)
      return DISP_E_BADPARAMCOUNT;
    const auto& input = parameters->rgvarg[0];
    if (input.vt != VT_BSTR || !input.bstrVal) return DISP_E_TYPEMISMATCH;
    const auto length = ::SysStringLen(input.bstrVal);
    if (length == 0 || length > kMaximumBatchItems * 11) return E_INVALIDARG;
    try {
      // Canonical positive decimal identifiers only. Validate the whole request
      // before admitting work; duplicates must never issue duplicate IOCTLs.
      std::vector<std::uint32_t> ids;
      std::uint32_t value = 0;
      bool digit_seen = false;
      for (UINT i = 0; i <= length; ++i) {
        const wchar_t ch = i == length ? L',' : input.bstrVal[i];
        if (ch == L',') {
          if (!digit_seen || value == 0 || ids.size() == kMaximumBatchItems ||
              std::find(ids.begin(), ids.end(), value) != ids.end()) return E_INVALIDARG;
          ids.push_back(value);
          value = 0; digit_seen = false;
        } else {
          if (ch < L'0' || ch > L'9' || (!digit_seen && ch == L'0')) return E_INVALIDARG;
          const auto digit = static_cast<std::uint32_t>(ch - L'0');
          if (value > (UINT32_MAX - digit) / 10) return E_INVALIDARG;
          value = value * 10 + digit; digit_seen = true;
        }
      }
      if (creation_work_->interrupted.load() || controller_.state() != CardProductUiState::authenticated)
        return E_ACCESSDENIED;
      if (reorder) {
        controller_.reorder_containers(ids);
        return complete_selection(true, result);
      }
      const auto available = controller_.batch_targets();
      std::vector<BatchItem> plan;
      plan.reserve(ids.size());
      for (const auto id : ids) {
        const auto entry = std::find_if(available.begin(), available.end(),
            [id](const auto& target) { return target.id == id; });
        if (entry == available.end()) return E_INVALIDARG;
        plan.push_back({*entry, 0});
      }
      if (creation_thread_.joinable()) creation_thread_.join();
      const auto epoch = creation_work_->card_epoch.load();
      {
        std::lock_guard lock(batch_mutex_);
        batch_items_ = plan;
        batch_epoch_ = epoch;
        batch_mount_ = mount;
        batch_state_ = 1;
        batch_completed_ = 0;
        batch_current_ = 0;
        batch_mode_ = true;
      }
      creation_work_->finish();
      creation_work_->volume_operation.store(true);
      volume_status_.store(3); // Batch outcomes are never a single-volume force grant.
      creation_work_->running.store(true);
      creation_thread_ = std::thread([this, plan = std::move(plan), mount, force, epoch] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto interrupted = [this, epoch] {
          return creation_work_->interrupted.load() || creation_work_->card_epoch.load() != epoch;
        };
        for (std::size_t i = 0; i < plan.size(); ++i) {
          if (interrupted()) break;
          {
            std::lock_guard lock(batch_mutex_);
            batch_current_ = i + 1;
            batch_items_[i].outcome = 1;
          }
          unsigned int outcome = 3;
          if (SUCCEEDED(com)) {
            try {
              if (plan[i].target.mounted == mount) outcome = 11;
              else {
                if (mount) controller_.mount_container(plan[i].target.id);
                else if (force) controller_.force_unmount_container(plan[i].target.id);
                else controller_.unmount_container(plan[i].target.id);
                outcome = 2;
              }
            } catch (const std::system_error& error) {
              outcome = volume_failure_status(error.code());
            } catch (...) {} // One failed item does not stop an independent request.
          }
          {
            std::lock_guard lock(batch_mutex_);
            batch_items_[i].outcome = interrupted() ? 10 : outcome;
            batch_completed_ = i + 1;
          }
        }
        if (SUCCEEDED(com)) ::CoUninitialize();
        {
          std::lock_guard lock(batch_mutex_);
          batch_state_ = interrupted() ? 4U : SUCCEEDED(com) ? 2U : 3U;
          batch_current_ = 0;
          // Removed-card snapshots must not retain container names for a later login.
          if (interrupted()) batch_items_.clear();
        }
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      creation_work_->running.store(false);
      clear_batch();
      return E_FAIL;
    }
  }

  HRESULT invoke_batch_query(DISPID member, DISPPARAMS* parameters, VARIANT* result) noexcept {
    const bool progress = member == kBatchProgressId;
    if (!parameters || parameters->cNamedArgs != 0 || parameters->cArgs != (progress ? 0U : 1U) ||
        (!progress && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
    std::uint32_t index = 0;
    if (!progress && !exact_uint32(parameters->rgvarg[0], index)) return DISP_E_TYPEMISMATCH;
    if (!result) return E_POINTER;
    try {
      if (!batch_mode_) return E_UNEXPECTED;
      if (batch_epoch_ != creation_work_->card_epoch.load() || creation_work_->interrupted.load() ||
          (!creation_work_->running.load() && controller_.state() != CardProductUiState::authenticated)) {
        if (!creation_work_->running.load()) clear_batch();
        return E_ACCESSDENIED;
      }
      std::lock_guard lock(batch_mutex_);
      if (progress) {
        const auto current_id = batch_current_ == 0 ? 0 : batch_items_[batch_current_ - 1].target.id;
        const std::wstring packed = std::to_wstring(batch_state_) + L"," + (batch_mount_ ? L"0," : L"1,") +
            std::to_wstring(batch_items_.size()) + L"," + std::to_wstring(batch_completed_) + L"," +
            std::to_wstring(batch_current_) + L"," + std::to_wstring(current_id);
        return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
      }
      if (index >= batch_items_.size()) return E_INVALIDARG;
      const auto& entry = batch_items_[index];
      if (member == kBatchItemNameId)
        return complete_string(entry.target.name.c_str(), static_cast<UINT>(entry.target.name.size()), result);
      ::VariantInit(result); result->vt = VT_UI4;
      result->ulVal = member == kBatchItemIdId ? entry.target.id : entry.outcome;
      return S_OK;
    } catch (...) { return E_FAIL; }
  }

  HRESULT begin_volume_operation(DISPID member, DISPPARAMS* parameters,
                                 VARIANT* result, UINT* argument_error) noexcept {
    const bool force = member == kBeginForceUnmountLastId;
    if (force) {
      if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
      if (volume_status_.load() != 4 || last_volume_epoch_ != creation_work_->card_epoch.load() ||
          (last_volume_member_ != kBeginUnmountId && last_volume_member_ != kBeginUnmountContainerId))
        return E_ACCESSDENIED;
      member = last_volume_member_;
    }
    // "all" is the cleanup exemption below, not merely the argument shape:
    // unmounting everything stays reachable once the card is gone, while
    // mounting everything is an ordinary authenticated action that happens to
    // take no identifier. Keeping them apart is what stops beginMountAll from
    // inheriting that exemption.
    const bool all = member == kBeginUnmountId;
    const bool no_id = all || member == kBeginMountAllId;
    if (!parameters || parameters->cNamedArgs != 0 ||
        parameters->cArgs != (no_id || force ? 0U : 1U) ||
        (!no_id && !force && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
    std::uint32_t id = force ? last_volume_id_ : 0;
    if (!no_id && !force && (!exact_uint32(parameters->rgvarg[0], id) || id == 0)) {
      if (argument_error) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      // Cleanup remains available in the unavailable state. A removed card
      // must be processed by the window before any new worker is admitted.
      if (creation_work_->interrupted.load()) return E_PENDING;
      if (!all && controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
      if (creation_thread_.joinable()) creation_thread_.join();
      last_volume_member_ = member;
      last_volume_id_ = id;
      last_volume_epoch_ = creation_work_->card_epoch.load();
      creation_work_->finish(); // Volume operations cannot be cancelled.
      creation_work_->volume_operation.store(true);
      volume_status_.store(1);
      creation_work_->running.store(true);
      creation_thread_ = std::thread([this, member, id, force] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        unsigned int status = 3;
        try {
          if (FAILED(com)) throw std::runtime_error("worker COM initialization failed");
          if (!creation_work_->interrupted.load()) {
            if (force && member == kBeginUnmountContainerId) controller_.force_unmount_container(id);
            else if (force) controller_.force_unmount_user_active();
            else if (member == kBeginMountAdoptedId) controller_.mount_container(id);
            else if (member == kBeginMountAllId) controller_.mount_all();
            else if (member == kBeginUnmountContainerId) controller_.unmount_container(id);
            else controller_.unmount();
            status = 2;
          }
        } catch (const std::system_error& error) {
          status = volume_failure_status(error.code());
        } catch (...) {}
        if (SUCCEEDED(com)) ::CoUninitialize();
        // A removal during an IOCTL is handled by the deferred lifecycle path.
        // Do not present its late result as a successful user operation.
        if (creation_work_->interrupted.load()) status = 10;
        volume_status_.store(status);
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      volume_status_.store(3);
      creation_work_->running.store(false);
      return E_FAIL;
    }
  }

  template <typename Operation>
  HRESULT begin_creation(Operation operation, VARIANT* result) {
    if (creation_work_->interrupted.load() || controller_.state() != CardProductUiState::authenticated) return E_FAIL;
    if (creation_thread_.joinable()) creation_thread_.join();
    creation_progress_.store(0);
    creation_flags_.store(0);
    creation_status_.store(1);
    creation_work_->volume_operation.store(false);
    creation_work_->begin();
    creation_work_->running.store(true);
    try {
      creation_thread_ = std::thread([this, operation = std::move(operation)]() mutable {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        unsigned int status = 3;
        try {
          if (FAILED(com)) throw std::runtime_error("worker COM initialization failed");
          controller_.set_creation_cancellation(
              [this]() { return creation_work_->cancellation_requested(); });
          controller_.set_creation_feedback([this](unsigned int stage, unsigned int percent, wchar_t drive) {
            if (stage == 4 && creation_work_->interrupted.load()) throw std::runtime_error("card removed during creation");
            // Both session creation paths emit stage 4 before publishing,
            // inside their staging-file cleanup scope. Close the cancel gate
            // before exposing this stage, and abort if a request won the race.
            if (stage == 4 && !creation_work_->begin_finalization()) {
              throw std::system_error(volume_creation::make_error_code(
                  volume_creation::CreationError::operation_cancelled));
            }
            if (stage == 1 || stage == 2) creation_flags_.fetch_or(1U << (stage - 1));
            creation_progress_.store((static_cast<std::uint64_t>(stage) << 32) |
                (static_cast<std::uint64_t>(percent) << 16) | static_cast<unsigned int>(drive));
          });
          operation();
          status = 2;
        } catch (const application::ContainerCreationCleanupError&) {
          status = 5;
        } catch (const std::system_error& error) {
          // A request alone proves neither cancellation nor successful cleanup.
          // Only the creator's confirmed outcome may claim cancellation.
          if (error.code() == volume_creation::make_error_code(
                  volume_creation::CreationError::operation_cancelled)) {
            status = 4;
          } else if (error.code() == volume_creation::make_error_code(
                         volume_creation::CreationError::cleanup_failed)) {
            status = 5;
          }
        } catch (...) {}
        try { controller_.set_creation_feedback({}); } catch (...) {}
        try { controller_.set_creation_cancellation({}); } catch (...) {}
        if (SUCCEEDED(com)) ::CoUninitialize();
        creation_status_.store(status);
        creation_work_->finish();
        creation_work_->running.store(false);
      });
    } catch (...) {
      creation_status_.store(3);
      creation_work_->finish();
      creation_work_->running.store(false);
      return E_FAIL;
    }
    return complete_selection(true, result);
  }
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
                                  UINT* const argument_error,
                                  bool background = false) noexcept {
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
      if (background) return begin_creation([this, name = std::wstring(name), directory = std::wstring(directory), size_mib]() mutable {
        LocationStringWiper name_wiper(name);
        LocationStringWiper directory_wiper(directory);
        controller_.create_container(name, directory, size_mib);
      }, result);
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
      UINT* const argument_error, bool background = false) noexcept {
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
      if (background) return begin_creation([this, name = std::wstring(name), directory = std::wstring(directory),
          outer_size_mib, hidden_size_mib, second = std::move(second), confirmation = std::move(confirmation)]() mutable {
        LocationStringWiper name_wiper(name);
        LocationStringWiper directory_wiper(directory);
        controller_.create_container_b(name, directory, outer_size_mib, hidden_size_mib,
                                        second.bytes(), confirmation.bytes());
      }, result);
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

  // Four arguments: the container, the directory the page is looking at, the
  // entry it picked, and the location the export goes to. The last one is a
  // path on this machine, so it is wiped with the rest.
  HRESULT invoke_export_container_entry(
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
    std::wstring_view destination_directory;
    if (!exact_uint32(parameters->rgvarg[3], id) ||
        !exact_wstring(parameters->rgvarg[2], relative_directory) ||
        !exact_wstring(parameters->rgvarg[1], name) ||
        !exact_wstring(parameters->rgvarg[0], destination_directory)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.export_container_entry(id, relative_directory, name,
                                         destination_directory);
      return complete_selection(true, result);
    } catch (const std::invalid_argument&) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      return E_FAIL;
    }
  }

  // Two arguments: the container, and where its file goes. The container is
  // named by identifier here as everywhere else, so the page never learns
  // where the container itself lives.
  HRESULT invoke_export_container_file(
      DISPPARAMS* const parameters, VARIANT* const result,
      UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 2 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t id = 0;
    std::wstring_view destination_directory;
    if (!exact_uint32(parameters->rgvarg[1], id) ||
        !exact_wstring(parameters->rgvarg[0], destination_directory)) {
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      controller_.export_container_file(id, destination_directory);
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
  IUiWindowSizeSink* window_size_sink_{};
  CreationWorkState local_creation_work_;
  CreationWorkState* creation_work_;
  std::thread creation_thread_;
  std::atomic<unsigned int> volume_status_{0};
  bool batch_mode_{false}; // Accessed only by the COM/window thread.
  std::mutex batch_mutex_;
  std::vector<BatchItem> batch_items_;
  std::uint64_t batch_epoch_{};
  bool batch_mount_{};
  unsigned int batch_state_{};
  std::size_t batch_completed_{};
  std::size_t batch_current_{};
  DISPID last_volume_member_{0};
  std::uint32_t last_volume_id_{0};
  std::uint64_t last_volume_epoch_{0};
  std::atomic<unsigned int> creation_status_{0};
  std::atomic<std::uint64_t> creation_progress_{0};
  std::atomic<unsigned int> creation_flags_{0};
};

}  // namespace

HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* const close_sink,
    IDispatch** const result,
    IUiWindowSizeSink* const window_size_sink,
    CreationWorkState* const creation_work) noexcept {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  auto* const object =
      new (std::nothrow) CardProductWebViewHostObject(controller, close_sink,
                                                   window_size_sink, creation_work);
  if (object == nullptr) return E_OUTOFMEMORY;
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
