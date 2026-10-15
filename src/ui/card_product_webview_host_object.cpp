#include "usb_vault/ui/card_product_webview_host_object.h"
#include "usb_vault/diagnostics/debug_log.h"
#include "usb_vault/application/container_index_v1.h"
#include "usb_vault/backup/export_package_v1.h"
#include "usb_vault/security/confirmed_password.h"
#include "usb_vault/ui/page_event_report.h"

#include <chrono>

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
#include <system_error>
#include <vector>
#include <thread>
#include <mutex>

namespace usb_vault::ui {
namespace {

constexpr DISPID kPartitionBeginId = 100;
constexpr DISPID kPartitionStatusId = 101;
constexpr DISPID kPartitionListId = 102;
constexpr DISPID kPartitionSelectId = 103;
constexpr DISPID kPartitionSelectedId = 104;
constexpr DISPID kPartitionClearId = 105;
constexpr DISPID kPartitionToolId = 106;
constexpr DISPID kPartitionCheckId = 107;
constexpr DISPID kPartitionCheckStatusId = 108;
constexpr DISPID kPartitionCreateId = 109;
constexpr DISPID kGetContainerPartitionId = 110;
constexpr DISPID kGetContainerConversionId = 111;
constexpr DISPID kGetContainerVolumeUnavailableId = 121;
constexpr DISPID kReportPageEventId = 122;
constexpr DISPID kBeginForceUnmountForExitId = 123;
constexpr DISPID kDismissCloseConfirmationId = 124;
constexpr DISPID kSetExitHotkeyCaptureId = 130;
constexpr DISPID kGetFileHidingId = 133;
constexpr DISPID kSetFileHidingId = 134;
constexpr DISPID kSetContainerFileHiddenId = 135;
constexpr DISPID kGetContainerFileHiddenId = 136;
constexpr DISPID kGetContainerExtensionId = 131;
constexpr DISPID kSetContainerExtensionId = 132;
constexpr DISPID kGetExitHotkeyId = 125;
constexpr DISPID kSetExitHotkeyId = 126;
constexpr DISPID kGetCloseToTrayId = 127;
constexpr DISPID kSetCloseToTrayId = 128;
constexpr DISPID kRenameContainerId = 129;
constexpr DISPID kPartitionConvertId = 112;
constexpr DISPID kPartitionResumeId = 113;
constexpr DISPID kPartitionDisplayId = 114;
constexpr DISPID kCreationErrorId = 115;
constexpr DISPID kBitLockerProgressId = 116;
constexpr DISPID kBitLockerConfirmId = 117;
constexpr DISPID kBitLockerHelpId = 118;
constexpr DISPID kPartitionPairId = 119;
constexpr DISPID kOpenCardBackupId = 230;
// P5.9 encrypted transfers run on the shared session worker; while one runs,
// only its status and its cancel are answered.
constexpr DISPID kBeginEncryptedExportId = 140;
constexpr DISPID kEncryptedTransferStatusId = 141;
constexpr DISPID kCancelEncryptedTransferId = 142;
bool is_encrypted_transfer_query(DISPID member) noexcept {
  return member == kEncryptedTransferStatusId || member == kCancelEncryptedTransferId;
}
constexpr DISPID kReturnToLoginId = 98;
constexpr DISPID kExitApplicationId = 99;
constexpr DISPID kCanLoginResetId = 90;
constexpr DISPID kBeginLoginResetPreparationId = 91;
constexpr DISPID kLoginResetStatusId = 92;
constexpr DISPID kLoginResetTicketId = 93;
constexpr DISPID kLoginResetDrivesId = 94;
constexpr DISPID kLoginResetMountsKnownId = 95;
constexpr DISPID kCancelLoginResetId = 96;
constexpr DISPID kBeginConfirmLoginResetId = 97;
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

constexpr DISPID kDirectoryErrorId = 120;
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
constexpr DISPID kOpenConfigurationId = 70;
constexpr DISPID kGetStartupViewId = 71;
constexpr DISPID kRefreshDecoyCandidatesId = 89;
constexpr DISPID kBeginDecoySurveyId = 72;
constexpr DISPID kDecoySurveyProgressId = 73;
constexpr DISPID kDecoySurveyIdId = 74;
constexpr DISPID kCancelDecoySurveyId = 75;
constexpr DISPID kBeginOpenDecoyMaintenanceId = 76;
constexpr DISPID kBeginMeasureDecoySpaceId = 77;
constexpr DISPID kDecoySpaceId = 78;
constexpr DISPID kBeginEndDecoyMaintenanceId = 79;
constexpr DISPID kSetExternalDropTargetId = 80;
constexpr DISPID kClearExternalDropTargetId = 81;
constexpr DISPID kCancelExternalDropId = 82;
constexpr DISPID kExternalDropStatusId = 83;
constexpr DISPID kDecoyWriteBlockedId = 84;
constexpr DISPID kSelectExternalFilesId = 85;
constexpr DISPID kExternalDropBoundsId = 87;
constexpr DISPID kExternalDropButtonBoundsId = 88;
constexpr DISPID kExternalFileProgressId = 86;
// The page asks one question before anything else: which surface the host
// opened it for. This object only ever serves the product session; setup has
// its own bridge with no card, volume or container member on it.
constexpr wchar_t kSessionView[] = L"session";
constexpr std::size_t kMaximumBatchItems = 1024;
bool is_batch_query(DISPID member) noexcept {
  return member >= kBatchProgressId && member <= kBatchItemOutcomeId;
}

// Readable while the survey walks the containers. Cancelling is one of them:
// unlike a volume operation, a walk can be called off between containers.
bool is_survey_query(DISPID member) noexcept {
  return member == kDecoySurveyProgressId || member == kDecoySurveyIdId ||
         member == kCancelDecoySurveyId;
}
constexpr DISPID kBeginForceUnmountLastId = 55;
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

// Why the last listing failed. One catch-all left the page telling the user a
// folder was gone whenever anything went wrong -- an unreachable volume, a
// refused path, a container that is simply not mounted -- so the reasons a
// person can act on differently are separated here.
enum class DirectoryFailure : unsigned int {
  none = 0,
  not_mounted = 1,
  unknown_container = 2,
  volume_unreachable = 3,
  directory_missing = 4,
  access_denied = 5,
  io_error = 6,
  contents_refused = 7,
  unknown = 8,
};

// What the page may read as a creation failure: a CreationError or nothing.
//
// CreationError runs 1..19, which collides densely with errno and Win32. A
// duplicate container name arrives as errc::file_exists, whose value is 17 --
// exactly CreationError::io_error -- so storing a foreign code unchanged had
// the page name a cause that was not the one, and name it confidently. Only
// this category is authoritative here; the one generic condition with an exact
// counterpart is translated, and anything else is left for the caller to
// report as undetermined rather than guessed at.
[[nodiscard]] unsigned int creation_failure_code(const std::error_code& code) noexcept {
  if (code.category() == volume_creation::creation_error_category()) {
    return static_cast<unsigned int>(code.value());
  }
  if (code == std::errc::file_exists) {
    return static_cast<unsigned int>(volume_creation::CreationError::target_exists);
  }
  return 0;
}

constexpr wchar_t kListContainerDirectoryName[] = L"listContainerDirectory";
constexpr wchar_t kDirectoryErrorName[] = L"getDirectoryError";
constexpr wchar_t kDeleteContainerEntryName[] = L"deleteContainerEntry";
constexpr wchar_t kRenameContainerEntryName[] = L"renameContainerEntry";
constexpr wchar_t kCreateContainerDirectoryName[] = L"createContainerDirectory";
constexpr wchar_t kMoveContainerEntryName[] = L"moveContainerEntry";
constexpr wchar_t kCopyContainerEntryName[] = L"copyContainerEntry";
constexpr wchar_t kExportContainerEntryName[] = L"exportContainerEntry";
constexpr wchar_t kExportContainerFileName[] = L"exportContainerFile";
constexpr wchar_t kSetExternalDropTargetName[] = L"setExternalDropTarget";
constexpr wchar_t kClearExternalDropTargetName[] = L"clearExternalDropTarget";
constexpr wchar_t kCancelExternalDropName[] = L"cancelExternalDrop";
constexpr wchar_t kExternalDropStatusName[] = L"getExternalDropStatus";

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


// ---- Test-phase runtime log for page calls --------------------------------
// The page's name for a member, taken from the same table GetIDsOfNames uses.
[[nodiscard]] const char* member_name(const DISPID member) noexcept {
  switch (member) {
    case kBeginConfirmLoginResetId: return "beginConfirmLoginReset";
    case kBeginCreateBId: return "beginCreateContainerBMib";
    case kBeginCreateId: return "beginCreateContainerMib";
    case kRefreshDecoyCandidatesId: return "refreshDecoyCandidates";
    case kBeginDecoySurveyId: return "beginDecoySurvey";
    case kBeginEndDecoyMaintenanceId: return "beginEndDecoyMaintenance";
    case kBeginForceUnmountBatchId: return "beginForceUnmountBatch";
    case kBeginForceUnmountLastId: return "beginForceUnmountLast";
    case kBeginLoginResetPreparationId: return "beginLoginResetPreparation";
    case kBeginMeasureDecoySpaceId: return "beginMeasureDecoySpace";
    case kBeginMountAdoptedId: return "beginMountAdoptedContainer";
    case kBeginMountBatchId: return "beginMountBatch";
    case kBeginOpenDecoyMaintenanceId: return "beginOpenDecoyMaintenance";
    case kPartitionCheckId: return "beginPartitionCheck";
    case kPartitionConvertId: return "beginPartitionConversion";
    case kPartitionCreateId: return "beginPartitionCreate";
    case kPartitionPairId: return "beginPartitionPair";
    case kPartitionBeginId: return "beginPartitionRefresh";
    case kPartitionResumeId: return "beginPartitionResume";
    case kBeginUnmountId: return "beginUnmount";
    case kBeginUnmountBatchId: return "beginUnmountBatch";
    case kBeginUnmountContainerId: return "beginUnmountContainer";
    case kCanLoginResetId: return "canLoginReset";
    case kCanResetCardRecordId: return "canResetCardRecord";
    case kCancelCreationId: return "cancelContainerCreation";
    case kCancelDecoySurveyId: return "cancelDecoySurvey";
    case kCancelExternalDropId: return "cancelExternalDrop";
    case kCancelLoginResetId: return "cancelLoginReset";
    case kChooseContainerDirectoryId: return "chooseContainerDirectory";
    case kClearExternalDropTargetId: return "clearExternalDropTarget";
    case kClearOperationHistoryId: return "clearOperationHistory";
    case kPartitionClearId: return "clearPartitions";
    case kBitLockerConfirmId: return "confirmPartitionBitLocker";
    case kCopyContainerEntryId: return "copyContainerEntry";
    case kCreateContainerBMibId: return "createContainerBMib";
    case kCreateContainerDirectoryId: return "createContainerDirectory";
    case kCreateContainerMibId: return "createContainerMib";
    case kDeleteContainerEntryId: return "deleteContainerEntry";
    case kExitApplicationId: return "exitApplication";
    case kBeginForceUnmountForExitId: return "beginForceUnmountForExit";
    case kDismissCloseConfirmationId: return "dismissCloseConfirmation";
    case kGetExitHotkeyId: return "getExitHotkey";
    case kSetExitHotkeyId: return "setExitHotkey";
    case kGetCloseToTrayId: return "getCloseToTrayEnabled";
    case kSetCloseToTrayId: return "setCloseToTrayEnabled";
    case kGetContainerFileHiddenId: return "getContainerFileHidden";
    case kGetFileHidingId: return "getHideFileContainers";
    case kSetFileHidingId: return "setHideFileContainers";
    case kSetContainerFileHiddenId: return "setContainerFileHidden";
    case kGetContainerExtensionId: return "getContainerExtension";
    case kSetContainerExtensionId: return "setContainerExtension";
    case kExportContainerEntryId: return "exportContainerEntry";
    case kExportContainerFileId: return "exportContainerFile";
    case kForgetContainerId: return "forgetContainer";
    case kGetAutoMountId: return "getAutoMountEnabled";
    case kBatchItemIdId: return "getBatchItemId";
    case kBatchItemNameId: return "getBatchItemName";
    case kBatchItemOutcomeId: return "getBatchItemOutcome";
    case kBatchProgressId: return "getBatchProgress";
    case kGetContainerConversionId: return "getContainerConversion";
    case kGetContainerCountId: return "getContainerCount";
    case kGetContainerDriveLetterId: return "getContainerDriveLetter";
    case kGetContainerIdId: return "getContainerId";
    case kGetContainerMountedId: return "getContainerMounted";
    case kGetContainerNameId: return "getContainerName";
    case kGetContainerPartitionId: return "getContainerPartition";
    case kGetContainerSizeMibId: return "getContainerSizeMib";
    case kGetContainerVolumeUnavailableId: return "getContainerVolumeUnavailable";
    case kCreationErrorId: return "getCreationError";
    case kCreationProgressId: return "getCreationProgress";
    case kDecoySpaceId: return "getDecoyMaintenanceSpace";
    case kDecoyWriteBlockedId: return "getDecoyMaintenanceWriteBlocked";
    case kDecoySurveyIdId: return "getDecoySurveyId";
    case kDecoySurveyProgressId: return "getDecoySurveyProgress";
    case kDirectoryErrorId: return "getDirectoryError";
    case kExternalDropStatusId: return "getExternalDropStatus";
    case kExternalFileProgressId: return "getExternalFileProgress";
    case kGetForcePreferenceId: return "getForceUnmountEnabled";
    case kLoginResetDrivesId: return "getLoginResetDrives";
    case kLoginResetMountsKnownId: return "getLoginResetMountsKnown";
    case kLoginResetStatusId: return "getLoginResetStatus";
    case kLoginResetTicketId: return "getLoginResetTicket";
    case kGetMountImportedContainersId: return "getMountImportedContainers";
    case kGetOperationCompletedAtId: return "getOperationCompletedAtUnixMs";
    case kGetOperationContainerNameId: return "getOperationContainerName";
    case kGetOperationCountId: return "getOperationCount";
    case kGetOperationDriveLetterId: return "getOperationDriveLetter";
    case kGetOperationKindId: return "getOperationKind";
    case kGetOperationLoggingEnabledId: return "getOperationLoggingEnabled";
    case kGetOperationOutcomeId: return "getOperationOutcome";
    case kBitLockerProgressId: return "getPartitionBitLockerProgress";
    case kPartitionCheckStatusId: return "getPartitionCheckStatus";
    case kPartitionDisplayId: return "getPartitionDisplay";
    case kPartitionListId: return "getPartitionList";
    case kPartitionStatusId: return "getPartitionRefreshStatus";
    case kGetRememberContainerPathsId: return "getRememberContainerPaths";
    case kPartitionSelectedId: return "getSelectedPartition";
    case kGetSessionStateId: return "getSessionState";
    case kGetStartupViewId: return "getStartupView";
    case kVolumeOperationStatusId: return "getVolumeOperationStatus";
    case kGetWindowSizeId: return "getWindowSize";
    case kHasActiveSessionId: return "hasActiveSession";
    case kListContainerDirectoryId: return "listContainerDirectory";
    case kMountAdoptedContainerId: return "mountAdoptedContainer";
    case kMountContainerId: return "mountContainer";
    case kMoveContainerEntryId: return "moveContainerEntry";
    case kBitLockerHelpId: return "openBitLockerHelp";
    case kOpenConfigurationId: return "openConfiguration";
    case kOpenCardBackupId: return "openCardBackup";
    case kBeginEncryptedExportId: return "beginEncryptedExport";
    case kEncryptedTransferStatusId: return "encryptedTransferStatus";
    case kCancelEncryptedTransferId: return "cancelEncryptedTransfer";
    case kPartitionToolId: return "openDiskManagement";
    case kRenameContainerEntryId: return "renameContainerEntry";
    case kReorderContainersId: return "reorderContainers";
    case kRenameContainerId: return "renameContainer";
    case kResetCardRecordId: return "resetCardRecord";
    case kReturnToLoginId: return "returnToLogin";
    case kSelectExternalFilesId: return "selectExternalFiles";
    case kPartitionSelectId: return "selectPartition";
    case kSetAutoMountId: return "setAutoMountEnabled";
    case kExternalDropBoundsId: return "setExternalDropBounds";
    case kExternalDropButtonBoundsId: return "setExternalDropButtonBounds";
    case kSetExternalDropTargetId: return "setExternalDropTarget";
    case kSetForcePreferenceId: return "setForceUnmountEnabled";
    case kSetMountImportedContainersId: return "setMountImportedContainers";
    case kSetOperationLoggingEnabledId: return "setOperationLoggingEnabled";
    case kSetRememberContainerPathsId: return "setRememberContainerPaths";
    case kSetWindowSizeId: return "setWindowSize";
    case kSubmitInitializationPasswordBytesId: return "submitInitializationPasswordBytes";
    case kSubmitLoginPasswordBytesId: return "submitLoginPasswordBytes";
    case kUnmountId: return "unmount";
    case kUnmountContainerId: return "unmountContainer";
    default: return nullptr;
  }
}

[[nodiscard]] std::string member_text(const DISPID member) {
  const char* const name = member_name(member);
  return name ? std::string(name) : "成员 " + std::to_string(member);
}

// Polling getters run many times a second while work is in flight. They are
// recorded only when they fail for a reason other than "still busy".
[[nodiscard]] bool polled_member(const DISPID member) noexcept {
  if (member == kRefreshDecoyCandidatesId) return true;
  const char* const name = member_name(member);
  if (name == nullptr) return false;
  const std::string_view text(name);
  return text.starts_with("get") || text.starts_with("has") || text.starts_with("is") ||
         text.ends_with("Status") || text.ends_with("Progress") || text.ends_with("Ticket") ||
         text.ends_with("Drives") || text.ends_with("MountsKnown") || text == "canLoginReset" ||
         text == "canResetCardRecord";
}

// A byte array is how a password crosses the bridge, so only its size is kept.
[[nodiscard]] std::string variant_text(const VARIANT& value) {
  if ((value.vt & VT_ARRAY) != 0) {
    const SAFEARRAY* const array = (value.vt & VT_BYREF) ? *value.pparray : value.parray;
    ULONG elements = 0;
    if (array != nullptr && array->cDims == 1) elements = array->rgsabound[0].cElements;
    return "字节数组 " + std::to_string(elements) + " 项";
  }
  switch (value.vt) {
    case VT_EMPTY: return "空";
    case VT_BOOL: return value.boolVal == VARIANT_FALSE ? "false" : "true";
    case VT_I4: return std::to_string(value.lVal);
    case VT_UI4: return std::to_string(value.ulVal);
    case VT_I2: return std::to_string(value.iVal);
    case VT_UI2: return std::to_string(value.uiVal);
    case VT_I8: return std::to_string(value.llVal);
    case VT_UI8: return std::to_string(value.ullVal);
    case VT_R8: return std::to_string(value.dblVal);
    case VT_INT: return std::to_string(value.intVal);
    case VT_UINT: return std::to_string(value.uintVal);
    case VT_BSTR: {
      if (value.bstrVal == nullptr) return "\"\"";
      const std::wstring_view text(value.bstrVal, ::SysStringLen(value.bstrVal));
      std::string shown = diagnostics::utf8(text.substr(0, 300));
      if (text.size() > 300) shown += "…（共 " + std::to_string(text.size()) + " 字符）";
      return "\"" + shown + "\"";
    }
    default: return "类型 " + std::to_string(value.vt);
  }
}

[[nodiscard]] std::string arguments_text(const DISPPARAMS* const parameters) {
  if (parameters == nullptr || parameters->cArgs == 0 || parameters->rgvarg == nullptr) return "无参数";
  std::string text = "参数 ";
  // COM passes arguments in reverse order.
  for (UINT index = parameters->cArgs; index > 0; --index) {
    if (index != parameters->cArgs) text += "，";
    text += variant_text(parameters->rgvarg[index - 1]);
  }
  return text;
}

class CardProductWebViewHostObject final : public IDispatch {
 public:
  bool resume_card_backup() noexcept {
    if (!backup_pending_ || configuration_pending_) return false;
    backup_pending_ = false;
    return true;
  }
  CardProductWebViewHostObject(
      CardProductUiSessionController& controller,
      IUiCloseRequestSink* const close_sink,
      IUiWindowSizeSink* const window_size_sink,
      CreationWorkState* const creation_work,
      IUiExternalDropTargetSink* const external_drop_sink,
      IUiPartitionServices* const partition_services,
      IUiExitHotkeySink* const exit_hotkey_sink) noexcept
      : controller_(controller), close_sink_(close_sink),
        window_size_sink_(window_size_sink),
        exit_hotkey_sink_(exit_hotkey_sink),
        creation_work_(creation_work ? creation_work : &local_creation_work_),
        external_drop_sink_(external_drop_sink),
        partition_services_(partition_services ? *partition_services : native_partition_services()) {}

  ~CardProductWebViewHostObject() {
    if (creation_thread_.joinable()) creation_thread_.join();
    controller_.cancel_login_reset();
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
    if (matches(names[0], L"reportPageEvent")) { ids[0] = kReportPageEventId; return S_OK; }
    if (matches(names[0], L"setWindowSize")) {
      ids[0] = kSetWindowSizeId;
      return S_OK;
    }
    if (matches(names[0], L"getWindowSize")) { ids[0] = kGetWindowSizeId; return S_OK; }
    if (matches(names[0], L"beginPartitionRefresh")) { ids[0] = kPartitionBeginId; return S_OK; }
    if (matches(names[0], L"getPartitionRefreshStatus")) { ids[0] = kPartitionStatusId; return S_OK; }
    if (matches(names[0], L"getPartitionDisplay")) { ids[0] = kPartitionDisplayId; return S_OK; }
    if (matches(names[0], L"getPartitionList")) { ids[0] = kPartitionListId; return S_OK; }
    if (matches(names[0], L"selectPartition")) { ids[0] = kPartitionSelectId; return S_OK; }
    if (matches(names[0], L"getSelectedPartition")) { ids[0] = kPartitionSelectedId; return S_OK; }
    if (matches(names[0], L"clearPartitions")) { ids[0] = kPartitionClearId; return S_OK; }
    if (matches(names[0], L"beginPartitionCheck")) { ids[0] = kPartitionCheckId; return S_OK; }
    if (matches(names[0], L"getPartitionCheckStatus")) { ids[0] = kPartitionCheckStatusId; return S_OK; }
    if (matches(names[0], L"getContainerConversion")) { ids[0]=kGetContainerConversionId; return S_OK; }
    if (matches(names[0], L"beginPartitionConversion")) { ids[0]=kPartitionConvertId; return S_OK; }
    if (matches(names[0], L"beginPartitionResume")) { ids[0]=kPartitionResumeId; return S_OK; }
    if (matches(names[0], L"getContainerPartition")) { ids[0] = kGetContainerPartitionId; return S_OK; }
    if (matches(names[0], L"getContainerVolumeUnavailable")) { ids[0] = kGetContainerVolumeUnavailableId; return S_OK; }
    if (matches(names[0], L"beginPartitionPair")) { ids[0] = kPartitionPairId; return S_OK; }
    if (matches(names[0], L"beginPartitionCreate")) { ids[0] = kPartitionCreateId; return S_OK; }
    if (matches(names[0], L"openDiskManagement")) { ids[0] = kPartitionToolId; return S_OK; }
    if (matches(names[0], L"returnToLogin")) { ids[0] = kReturnToLoginId; return S_OK; }
    if (matches(names[0], L"exitApplication")) { ids[0] = kExitApplicationId; return S_OK; }
    if (matches(names[0], L"beginForceUnmountForExit")) { ids[0] = kBeginForceUnmountForExitId; return S_OK; }
    if (matches(names[0], L"dismissCloseConfirmation")) { ids[0] = kDismissCloseConfirmationId; return S_OK; }
    if (matches(names[0], L"setExitHotkeyCapture")) { ids[0] = kSetExitHotkeyCaptureId; return S_OK; }
    if (matches(names[0], L"getExitHotkey")) { ids[0] = kGetExitHotkeyId; return S_OK; }
    if (matches(names[0], L"setExitHotkey")) { ids[0] = kSetExitHotkeyId; return S_OK; }
    if (matches(names[0], L"getCloseToTrayEnabled")) { ids[0] = kGetCloseToTrayId; return S_OK; }
    if (matches(names[0], L"setCloseToTrayEnabled")) { ids[0] = kSetCloseToTrayId; return S_OK; }
    if (matches(names[0], L"getContainerFileHidden")) { ids[0] = kGetContainerFileHiddenId; return S_OK; }
    if (matches(names[0], L"getHideFileContainers")) { ids[0] = kGetFileHidingId; return S_OK; }
    if (matches(names[0], L"setHideFileContainers")) { ids[0] = kSetFileHidingId; return S_OK; }
    if (matches(names[0], L"setContainerFileHidden")) { ids[0] = kSetContainerFileHiddenId; return S_OK; }
    if (matches(names[0], L"getContainerExtension")) { ids[0] = kGetContainerExtensionId; return S_OK; }
    if (matches(names[0], L"setContainerExtension")) { ids[0] = kSetContainerExtensionId; return S_OK; }
    if (matches(names[0], L"openCardBackup")) { ids[0] = kOpenCardBackupId; return S_OK; }
    if (matches(names[0], L"beginEncryptedExport")) { ids[0] = kBeginEncryptedExportId; return S_OK; }
    if (matches(names[0], L"encryptedTransferStatus")) { ids[0] = kEncryptedTransferStatusId; return S_OK; }
    if (matches(names[0], L"cancelEncryptedTransfer")) { ids[0] = kCancelEncryptedTransferId; return S_OK; }
    if (matches(names[0], L"openConfiguration")) { ids[0] = kOpenConfigurationId; return S_OK; }
    if (matches(names[0], L"getStartupView")) { ids[0] = kGetStartupViewId; return S_OK; }
    if (matches(names[0], L"getAutoMountEnabled")) { ids[0] = kGetAutoMountId; return S_OK; }
    if (matches(names[0], L"setAutoMountEnabled")) { ids[0] = kSetAutoMountId; return S_OK; }
    if (matches(names[0], L"reorderContainers")) { ids[0] = kReorderContainersId; return S_OK; }
    if (matches(names[0], L"renameContainer")) { ids[0] = kRenameContainerId; return S_OK; }
    if (matches(names[0], L"getForceUnmountEnabled")) { ids[0] = kGetForcePreferenceId; return S_OK; }
    if (matches(names[0], L"setForceUnmountEnabled")) { ids[0] = kSetForcePreferenceId; return S_OK; }
    if (matches(names[0], L"beginCreateContainerMib")) { ids[0] = kBeginCreateId; return S_OK; }
    if (matches(names[0], L"beginCreateContainerBMib")) { ids[0] = kBeginCreateBId; return S_OK; }
    if (matches(names[0], L"getPartitionBitLockerProgress")) { ids[0] = kBitLockerProgressId; return S_OK; }
    if (matches(names[0], L"confirmPartitionBitLocker")) { ids[0] = kBitLockerConfirmId; return S_OK; }
    if (matches(names[0], L"openBitLockerHelp")) { ids[0] = kBitLockerHelpId; return S_OK; }
    if (matches(names[0], L"getCreationError")) { ids[0] = kCreationErrorId; return S_OK; }
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
    if (matches(names[0], kDirectoryErrorName)) {
      ids[0] = kDirectoryErrorId;
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
    if (matches(names[0], L"beginUnmountContainer")) { ids[0] = kBeginUnmountContainerId; return S_OK; }
    if (matches(names[0], L"beginUnmount")) { ids[0] = kBeginUnmountId; return S_OK; }
    if (matches(names[0], L"beginForceUnmountLast")) { ids[0] = kBeginForceUnmountLastId; return S_OK; }
    if (matches(names[0], L"getVolumeOperationStatus")) { ids[0] = kVolumeOperationStatusId; return S_OK; }
    if (matches(names[0], L"refreshDecoyCandidates")) { ids[0] = kRefreshDecoyCandidatesId; return S_OK; }
    if (matches(names[0], L"beginDecoySurvey")) { ids[0] = kBeginDecoySurveyId; return S_OK; }
    if (matches(names[0], L"getDecoySurveyProgress")) { ids[0] = kDecoySurveyProgressId; return S_OK; }
    if (matches(names[0], L"getDecoySurveyId")) { ids[0] = kDecoySurveyIdId; return S_OK; }
    if (matches(names[0], L"cancelDecoySurvey")) { ids[0] = kCancelDecoySurveyId; return S_OK; }
    if (matches(names[0], L"beginOpenDecoyMaintenance")) { ids[0] = kBeginOpenDecoyMaintenanceId; return S_OK; }
    if (matches(names[0], L"beginMeasureDecoySpace")) { ids[0] = kBeginMeasureDecoySpaceId; return S_OK; }
    if (matches(names[0], L"getDecoyMaintenanceSpace")) { ids[0] = kDecoySpaceId; return S_OK; }
    if (matches(names[0], L"getDecoyMaintenanceWriteBlocked")) { ids[0] = kDecoyWriteBlockedId; return S_OK; }
    if (matches(names[0], L"beginEndDecoyMaintenance")) { ids[0] = kBeginEndDecoyMaintenanceId; return S_OK; }
    if (matches(names[0], L"getExternalFileProgress")) { ids[0] = kExternalFileProgressId; return S_OK; }
    if (matches(names[0], L"setExternalDropButtonBounds")) { ids[0] = kExternalDropButtonBoundsId; return S_OK; }
    if (matches(names[0], L"setExternalDropBounds")) { ids[0] = kExternalDropBoundsId; return S_OK; }
    if (matches(names[0], L"selectExternalFiles")) { ids[0] = kSelectExternalFilesId; return S_OK; }
    if (matches(names[0], kSetExternalDropTargetName)) { ids[0] = kSetExternalDropTargetId; return S_OK; }
    if (matches(names[0], kClearExternalDropTargetName)) { ids[0] = kClearExternalDropTargetId; return S_OK; }
    if (matches(names[0], kCancelExternalDropName)) { ids[0] = kCancelExternalDropId; return S_OK; }
    if (matches(names[0], kExternalDropStatusName)) { ids[0] = kExternalDropStatusId; return S_OK; }
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
    if (matches(names[0], L"canLoginReset")) { ids[0] = kCanLoginResetId; return S_OK; }
    if (matches(names[0], L"beginLoginResetPreparation")) { ids[0] = kBeginLoginResetPreparationId; return S_OK; }
    if (matches(names[0], L"getLoginResetStatus")) { ids[0] = kLoginResetStatusId; return S_OK; }
    if (matches(names[0], L"getLoginResetTicket")) { ids[0] = kLoginResetTicketId; return S_OK; }
    if (matches(names[0], L"getLoginResetDrives")) { ids[0] = kLoginResetDrivesId; return S_OK; }
    if (matches(names[0], L"getLoginResetMountsKnown")) { ids[0] = kLoginResetMountsKnownId; return S_OK; }
    if (matches(names[0], L"cancelLoginReset")) { ids[0] = kCancelLoginResetId; return S_OK; }
    if (matches(names[0], L"beginConfirmLoginReset")) { ids[0] = kBeginConfirmLoginResetId; return S_OK; }
    ids[0] = DISPID_UNKNOWN;
    return DISP_E_UNKNOWNNAME;
  }

  HRESULT STDMETHODCALLTYPE Invoke(DISPID member, REFIID iid, LCID locale, WORD flags,
                                   DISPPARAMS* parameters, VARIANT* result,
                                   EXCEPINFO* exception,
                                   UINT* argument_error) noexcept override {
    if (member == kReportPageEventId)
      return report_page_event(iid, flags, parameters, result, argument_error);
    if constexpr (!USB_VAULT_DEBUG_LOG) {
      return invoke_unlogged(member, iid, locale, flags, parameters, result, exception,
                             argument_error);
    } else {
      // Arguments are wiped by the call itself, so they are described first.
      std::string arguments;
      try {
        arguments = arguments_text(parameters);
      } catch (...) {
        USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      }
      const auto started = std::chrono::steady_clock::now();
      const HRESULT hr = invoke_unlogged(member, iid, locale, flags, parameters, result,
                                         exception, argument_error);
      // Keep protection observations in test logs to distinguish missed queries from driver state.
      const bool polled = polled_member(member) && member != kDecoyWriteBlockedId;
      if (!polled || (FAILED(hr) && hr != E_PENDING)) {
        std::string returned;
        try {
          if (SUCCEEDED(hr) && result != nullptr) returned = "；返回 " + variant_text(*result);
        } catch (...) {
          USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
        }
        USB_VAULT_TRACE("页面桥", "页面调用 " + member_text(member),
                        arguments + "；HRESULT " + diagnostics::hex_text(static_cast<std::uint32_t>(hr)) +
                            returned + "；耗时 " +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()) +
                            " 毫秒");
      }
      return hr;
    }
  }

  HRESULT invoke_unlogged(DISPID member, REFIID iid, LCID, WORD flags,
                          DISPPARAMS* parameters, VARIANT* result,
                          EXCEPINFO*, UINT* argument_error) noexcept {
    if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
    if ((flags & DISPATCH_METHOD) == 0) return DISP_E_MEMBERNOTFOUND;
    if (member == kSetExitHotkeyCaptureId) {
      if (!parameters || parameters->cArgs != 1 || parameters->cNamedArgs || !parameters->rgvarg) return DISP_E_BADPARAMCOUNT;
      if (parameters->rgvarg[0].vt != VT_BOOL) return DISP_E_TYPEMISMATCH;
      if (!exit_hotkey_sink_) return E_FAIL;
      exit_hotkey_sink_->set_capture(parameters->rgvarg[0].boolVal != VARIANT_FALSE);
      return S_OK;
    }
    if (configuration_pending_ || backup_pending_ || creation_work_->hotkey_exit_pending.load()) {
      DispatchTextWiper text_wiper(parameters);
      wipe_available_arguments(parameters, parameters ? parameters->cArgs : 0);
      return E_PENDING;
    }
    // Workers exclusively own the session. Only their atomic progress is
    // readable; cancellation belongs to creation, never volume operations.
    // A drop that arrived on the window owns the session exactly as a volume
    // operation does, so its progress and its cancel are readable throughout;
    // they carry no session state of their own and are answered from atomics.
    const bool external_drop_query =
        member == kExternalDropStatusId || member == kCancelExternalDropId || member == kExternalFileProgressId || member == kExternalDropBoundsId || member == kExternalDropButtonBoundsId;
    if (partition_mode_ && creation_work_->running.load() &&
        !external_drop_query && member != kPartitionStatusId && member != kPartitionCheckStatusId) {
      DispatchTextWiper text_wiper(parameters);
      wipe_available_arguments(parameters, parameters ? parameters->cArgs : 0);
      return E_PENDING;
    }
    if (creation_work_->running.load() && !external_drop_query &&
        !(partition_mode_ && (member == kPartitionStatusId || member == kPartitionCheckStatusId)) &&
        !(reset_mode_ && member == kLoginResetStatusId) &&
        (creation_work_->volume_operation.load()
             ? (batch_mode_    ? !is_batch_query(member)
                : survey_mode_ ? !is_survey_query(member)
                : encrypted_mode_.load() ? !is_encrypted_transfer_query(member)
                               : member != kVolumeOperationStatusId)
             : member != kCreationProgressId &&
                   member != kCancelCreationId && member != kBitLockerProgressId &&
                   member != kBitLockerConfirmId && member != kBitLockerHelpId)) {
      DispatchTextWiper text_wiper(parameters);
      wipe_available_arguments(parameters, parameters ? parameters->cArgs : 0);
      return E_PENDING;
    }

    if (!creation_work_->running.load() &&
        !((member >= kPartitionBeginId && member <= kPartitionCreateId) || member==kPartitionConvertId || member==kPartitionResumeId || member==kPartitionDisplayId || member==kPartitionPairId)) partition_mode_ = false;
    if (!creation_work_->running.load() &&
        (controller_.state() != CardProductUiState::authenticated ||
         partition_epoch_ != creation_work_->card_epoch.load())) clear_partitions();
    if ((member >= kPartitionBeginId && member <= kPartitionCreateId) || member==kPartitionConvertId || member==kPartitionResumeId || member==kPartitionDisplayId || member==kPartitionPairId)
      return invoke_partition(member, parameters, result);

    if (member >= kCanLoginResetId && member <= kBeginConfirmLoginResetId)
      return invoke_login_reset(member, parameters, result);

    // Observations may refresh the failure dialog. Any unrelated mutation
    // revokes the last cleanup target, so a stale page cannot force a new mount.
    switch (member) {
      case kBeginForceUnmountLastId: case kVolumeOperationStatusId:
      case kBeginForceUnmountForExitId: case kDismissCloseConfirmationId:
      case kBatchProgressId: case kBatchItemIdId: case kBatchItemNameId: case kBatchItemOutcomeId:
      case kDecoySurveyProgressId: case kDecoySurveyIdId: case kCancelDecoySurveyId:
      case kBeginOpenDecoyMaintenanceId: // Opening one is what a survey is for.
      case kRefreshDecoyCandidatesId:
      case kDecoySpaceId:
      case kDecoyWriteBlockedId:
      // Arming the window is not a session mutation: it says where a drop
      // would land and touches nothing a retry, a batch or a measurement is
      // holding, and the maintenance page re-arms on every directory change.
      case kSetExternalDropTargetId: case kClearExternalDropTargetId: case kExternalDropBoundsId: case kExternalDropButtonBoundsId:
      case kExternalDropStatusId: case kCancelExternalDropId: case kExternalFileProgressId:
      case kHasActiveSessionId: case kGetSessionStateId:
      case kGetContainerCountId: case kGetContainerIdId: case kGetContainerNameId:
      case kGetContainerMountedId: case kGetContainerSizeMibId: case kGetContainerDriveLetterId:
      case kGetContainerPartitionId: case kGetContainerConversionId:
      case kGetContainerVolumeUnavailableId:
      case kGetOperationCountId: case kGetOperationCompletedAtId: case kGetOperationKindId:
      case kGetOperationOutcomeId: case kGetOperationContainerNameId: case kGetOperationDriveLetterId:
      case kCanResetCardRecordId: case kGetRememberContainerPathsId:
      case kGetMountImportedContainersId: case kGetOperationLoggingEnabledId:
      case kGetForcePreferenceId: case kGetWindowSizeId:
      case kGetContainerFileHiddenId:
      case kGetFileHidingId:
      case kGetContainerExtensionId:
      case kGetExitHotkeyId:
      case kGetCloseToTrayId:
      case kGetStartupViewId:
      case kGetAutoMountId:
      case kSetForcePreferenceId: // Changing a preference must not discard retry results.
      case kListContainerDirectoryId:
      // Reading why the last listing failed must not discard retry results
      // either; it is the page asking about a failure it has already seen.
      case kDirectoryErrorId: break;
      default:
        creation_work_->dismiss_close_confirmation();
        last_volume_member_ = 0;
        clear_batch();
        clear_survey();
        clear_space();
        break;
    }
    switch (member) {
      case kDismissCloseConfirmationId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        creation_work_->dismiss_close_confirmation();
        return S_OK;
      case kGetCloseToTrayId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (result) {
          ::VariantInit(result); result->vt = VT_BOOL;
          result->boolVal = close_sink_ && close_sink_->close_to_tray() ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
      case kSetCloseToTrayId:
        if (!parameters || parameters->cArgs != 1 || parameters->cNamedArgs || !parameters->rgvarg) return DISP_E_BADPARAMCOUNT;
        if (parameters->rgvarg[0].vt != VT_BOOL) return DISP_E_TYPEMISMATCH;
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        return close_sink_ && close_sink_->set_close_to_tray(parameters->rgvarg[0].boolVal != VARIANT_FALSE) ? S_OK : E_FAIL;
      case kGetExitHotkeyId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        const auto settings = exit_hotkey_sink_ ? exit_hotkey_sink_->exit_hotkey() : application::ExitHotkeySettings{};
        if (result) {
          ::VariantInit(result); result->vt = VT_UI4;
          result->ulVal = (settings.enabled ? 65536U : 0U) | (settings.count << 8) | settings.key;
        }
        return S_OK;
      }
      case kSetExitHotkeyId: {
        if (!parameters || parameters->cArgs != 3 || parameters->cNamedArgs || !parameters->rgvarg) return DISP_E_BADPARAMCOUNT;
        std::uint32_t key{}, count{};
        if (parameters->rgvarg[2].vt != VT_BOOL ||
            !exact_uint32(parameters->rgvarg[1], key) || !exact_uint32(parameters->rgvarg[0], count)) return DISP_E_TYPEMISMATCH;
        const application::ExitHotkeySettings settings{parameters->rgvarg[2].boolVal != VARIANT_FALSE, key, count};
        if (!application::valid_exit_hotkey(settings)) return E_INVALIDARG;
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        return exit_hotkey_sink_ && exit_hotkey_sink_->set_exit_hotkey(settings) ? S_OK : E_FAIL;
      }
      case kBeginForceUnmountForExitId:
        return begin_volume_operation(member, parameters, result, argument_error);
      case kBeginMountBatchId: case kBeginUnmountBatchId:
      case kBeginForceUnmountBatchId:
        return begin_batch(member == kBeginMountBatchId, member == kBeginForceUnmountBatchId, parameters, result);
      case kReorderContainersId:
        return begin_batch(false, false, parameters, result, true);
      case kRenameContainerId:
        return invoke_rename_container(parameters, argument_error);
      case kGetStartupViewId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        ::VariantInit(result);
        return complete_string(kSessionView,
                               static_cast<UINT>(std::size(kSessionView) - 1),
                               result);
      case kReturnToLoginId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (controller_.state() != CardProductUiState::authenticated || controller_.mounted())
          return E_ACCESSDENIED;
        if (!close_sink_ || !close_sink_->request_login()) return E_FAIL;
        configuration_pending_ = true;
        return S_OK;
      case kExitApplicationId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!close_sink_) return E_FAIL;
        // The window's normal close guard owns cleanup and can refuse to exit.
        close_sink_->request_close();
        return S_OK;
      case kOpenCardBackupId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!close_sink_ || !close_sink_->request_card_backup()) return E_FAIL;
        controller_.cancel_login_reset();
        backup_pending_ = true;
        return S_OK;
      case kOpenConfigurationId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (controller_.state() == CardProductUiState::authenticated || controller_.mounted())
          return E_ACCESSDENIED;
        if (!close_sink_ || !close_sink_->request_configuration()) return E_FAIL;
        controller_.cancel_login_reset();
        configuration_pending_ = true;
        return S_OK;
      case kBatchProgressId: case kBatchItemIdId: case kBatchItemNameId: case kBatchItemOutcomeId:
        return invoke_batch_query(member, parameters, result);
      case kRefreshDecoyCandidatesId: {
        if (!parameters || parameters->cArgs != 0 || parameters->cNamedArgs != 0)
          return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        try {
          const auto ids = controller_.decoy_maintenance_candidates();
          std::wstring packed;
          for (const auto id : ids) {
            if (!packed.empty()) packed += L",";
            packed += std::to_wstring(id);
          }
          return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
        } catch (...) { return E_FAIL; }
      }
      case kBeginDecoySurveyId:
        return begin_decoy_survey(parameters, result, argument_error);
      case kDecoySurveyProgressId: case kDecoySurveyIdId: case kCancelDecoySurveyId:
        return invoke_survey_query(member, parameters, result);
      case kBeginMountAdoptedId:
      case kBeginUnmountContainerId:
      case kBeginUnmountId:
      case kBeginForceUnmountLastId:
      case kBeginOpenDecoyMaintenanceId:
      case kBeginMeasureDecoySpaceId:
      case kBeginEndDecoyMaintenanceId:
        return begin_volume_operation(member, parameters, result, argument_error);
      case kDecoyWriteBlockedId: {
        if (!parameters || parameters->cArgs != 1 || parameters->cNamedArgs != 0)
          return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        std::uint32_t id = 0;
        if (!exact_uint32(parameters->rgvarg[0], id) || !id) return DISP_E_TYPEMISMATCH;
        if (creation_work_->interrupted.load()) return E_ACCESSDENIED;
        try {
          const bool blocked = controller_.decoy_maintenance_write_blocked(id);
          if (blocked) warn_capacity_once(id, true);
          return complete_selection(blocked, result);
        }
        catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
      }
      case kDecoySpaceId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        if (creation_work_->interrupted.load() ||
            controller_.state() != CardProductUiState::authenticated) {
          clear_space();
          return E_ACCESSDENIED;
        }
        std::lock_guard lock(space_mutex_);
        if (space_epoch_ != creation_work_->card_epoch.load()) {
          space_ready_ = false;
          space_id_ = 0;
          return E_ACCESSDENIED;
        }
        if (!space_ready_ || space_id_ == 0) return E_UNEXPECTED;
        const std::wstring packed = std::to_wstring(space_total_) + L"," +
            std::to_wstring(space_free_) + L"," + std::to_wstring(space_safe_free_);
        return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
      }
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
      case kBitLockerProgressId:
      case kBitLockerConfirmId:
      case kBitLockerHelpId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!creation_work_->running.load()) {
          if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
          if (member != kBitLockerProgressId) return E_ACCESSDENIED;
          return complete_string(L"0,0,0", 5, result);
        }
        if (creation_work_->interrupted.load() || bitlocker_epoch_.load() != creation_work_->card_epoch.load()) return E_ACCESSDENIED;
        const auto phase = bitlocker_phase_.load();
        if (member == kBitLockerHelpId) return complete_selection(phase == 1 && partition_services_.open_bitlocker_help(), result);
        if (member == kBitLockerConfirmId) {
          bool expected = false;
          return complete_selection(phase == 1 && !creation_work_->cancellation_requested() &&
              bitlocker_consent_.compare_exchange_strong(expected, true), result);
        }
        const auto status = bitlocker_status_.load();
        const auto packed = std::to_wstring(phase) + L"," + std::to_wstring(status >> 8) + L"," + std::to_wstring(status & 255);
        return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
      }
      case kCreationErrorId: {
        if (!parameters || parameters->cArgs != 0 || parameters->cNamedArgs != 0) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        ::VariantInit(result); result->vt = VT_UI4;
        result->ulVal = creation_error_.load();
        return S_OK;
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
        } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
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
        } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
      }
      case kGetContainerFileHiddenId: {
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        if (!parameters || parameters->cNamedArgs || parameters->cArgs != 1 || !parameters->rgvarg) return DISP_E_BADPARAMCOUNT;
        std::uint32_t id = 0;
        if (!exact_uint32(parameters->rgvarg[0], id) || !id) return DISP_E_TYPEMISMATCH;
        if (!result) return E_POINTER;
        try {
          const bool hidden = controller_.container_file_hidden(id);
          ::VariantInit(result); result->vt = VT_BOOL; result->boolVal = hidden ? VARIANT_TRUE : VARIANT_FALSE;
          return S_OK;
        } catch (...) { return E_FAIL; }
      }
      case kGetFileHidingId:
      case kSetFileHidingId:
      case kSetContainerFileHiddenId: {
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        const bool read = member == kGetFileHidingId;
        const bool action = member == kSetContainerFileHiddenId;
        if (!parameters || parameters->cNamedArgs || parameters->cArgs != (read ? 0U : action ? 2U : 1U) ||
            (!read && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
        std::uint32_t id = 0;
        if (!read && parameters->rgvarg[0].vt != VT_BOOL) return DISP_E_TYPEMISMATCH;
        if (action && (!exact_uint32(parameters->rgvarg[1], id) || !id)) return DISP_E_TYPEMISMATCH;
        try {
          if (read) {
            if (!result) return E_POINTER;
            ::VariantInit(result); result->vt = VT_BOOL;
            result->boolVal = controller_.hide_new_file_containers() ? VARIANT_TRUE : VARIANT_FALSE;
          } else if (action) controller_.set_container_file_hidden(id, parameters->rgvarg[0].boolVal != VARIANT_FALSE);
          else controller_.set_hide_new_file_containers(parameters->rgvarg[0].boolVal != VARIANT_FALSE);
          return S_OK;
        } catch (...) { return E_FAIL; }
      }
      case kGetContainerExtensionId:
      case kSetContainerExtensionId: {
        if (controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        const bool write = member == kSetContainerExtensionId;
        if (!parameters || parameters->cNamedArgs != 0 || parameters->cArgs != (write ? 1U : 0U) ||
            (write && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
        std::uint32_t value = 0;
        if (write && (!exact_uint32(parameters->rgvarg[0], value) || value > 3)) {
          if (argument_error) *argument_error = 0;
          return DISP_E_TYPEMISMATCH;
        }
        try {
          if (write) controller_.set_container_extension_preference(value);
          else {
            if (!result) return E_POINTER;
            ::VariantInit(result); result->vt = VT_UI4;
            result->ulVal = controller_.container_extension_preference();
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
      // Naming the destination is all the page does about a shell drop. It
      // says which maintenance container and which directory it is showing;
      // what is dropped, and where it came from, it is never told.
      case kSetExternalDropTargetId: {
        if (!parameters || parameters->cArgs != 2 || parameters->cNamedArgs != 0 ||
            !parameters->rgvarg) return DISP_E_BADPARAMCOUNT;
        const DispatchTextWiper text_wiper(parameters);
        std::uint32_t id = 0;
        std::wstring_view relative_directory;
        if (!exact_uint32(parameters->rgvarg[1], id) || id == 0 ||
            !exact_wstring(parameters->rgvarg[0], relative_directory)) {
          if (argument_error) *argument_error = 0;
          return DISP_E_TYPEMISMATCH;
        }
        // A page that is not on the authenticated route cannot arm the window
        // to write into a volume, whatever it has cached about one.
        if (creation_work_->interrupted.load() ||
            controller_.state() != CardProductUiState::authenticated) {
          if (external_drop_sink_) external_drop_sink_->clear_target();
          return E_ACCESSDENIED;
        }
        return external_drop_sink_ &&
                       external_drop_sink_->set_target(id, relative_directory)
                   ? S_OK : E_ACCESSDENIED;
      }
      case kExternalDropBoundsId: case kExternalDropButtonBoundsId: {
        if (!parameters || parameters->cArgs != 4 || parameters->cNamedArgs || !parameters->rgvarg)
          return DISP_E_BADPARAMCOUNT;
        std::uint32_t values[4]{};
        for (unsigned i=0;i<4;++i)
          if (!exact_uint32(parameters->rgvarg[3-i], values[i]) || values[i]>32768) return DISP_E_TYPEMISMATCH;
        if (!external_drop_sink_) return E_FAIL;
        const int x=static_cast<int>(values[0]), y=static_cast<int>(values[1]);
        const int w=static_cast<int>(values[2]), h=static_cast<int>(values[3]);
        return (member == kExternalDropBoundsId ? external_drop_sink_->set_drop_bounds(x,y,w,h)
            : external_drop_sink_->set_drop_button_bounds(x,y,w,h)) ? S_OK : E_FAIL;
      }
      case kSelectExternalFilesId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (creation_work_->running.load() || creation_work_->interrupted.load() ||
            controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
        return complete_selection(external_drop_sink_ && external_drop_sink_->select_files(), result);
      case kClearExternalDropTargetId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (external_drop_sink_) external_drop_sink_->clear_target();
        return S_OK;
      case kCancelExternalDropId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        return complete_selection(
            external_drop_sink_ && external_drop_sink_->request_cancel(), result);
      case kExternalFileProgressId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        if (!external_drop_sink_) return E_ACCESSDENIED;
        ::VariantInit(result); result->vt = VT_UI4;
        result->ulVal = (std::min)(external_drop_sink_->file_progress(),99U);
        return S_OK;
      case kExternalDropStatusId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (!result) return E_POINTER;
        ::VariantInit(result);
        if (!external_drop_sink_) return E_ACCESSDENIED;
        try {
          const ExternalDropStatus status = external_drop_sink_->status();
          if (!creation_work_->running.load() && !creation_work_->interrupted.load() &&
              status.state == 3 && status.capacity_limited && status.run_id != 0 &&
              status.run_id != warned_import_run_ &&
              status.card_epoch == creation_work_->card_epoch.load() &&
              controller_.state() == CardProductUiState::authenticated) {
            // Retire the event before any modal loop can reenter the bridge.
            warned_import_run_ = status.run_id;
            try {
              static_cast<void>(controller_.decoy_maintenance_write_blocked(status.container_id));
              warn_capacity_once(status.container_id, false);
            } catch (...) {} // A stale target must not produce a new warning.
          }
          const std::wstring packed = std::to_wstring(status.state) + L"," +
              std::to_wstring(status.total) + L"," +
              std::to_wstring(status.completed) + L"," +
              std::to_wstring(status.failed);
          return complete_string(packed.c_str(),
                                 static_cast<UINT>(packed.size()), result);
        } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
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
      case kDirectoryErrorId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (result == nullptr) return E_POINTER;
        ::VariantInit(result);
        result->vt = VT_UI4;
        result->ulVal = directory_error_.load();
        return S_OK;
      }
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
      case kBeginEncryptedExportId:
        return begin_encrypted_export(parameters, result, argument_error);
      case kEncryptedTransferStatusId: {
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        if (result == nullptr) return E_POINTER;
        // A finished transfer belongs to the login that ran it. After the
        // card changes or the session ends, its outcome and size are gone,
        // so a later login cannot learn that an export happened.
        if (!creation_work_->running.load() &&
            (encrypted_epoch_.load() != creation_work_->card_epoch.load() ||
             creation_work_->interrupted.load() ||
             controller_.state() != CardProductUiState::authenticated)) {
          encrypted_state_.store(0);
          encrypted_done_.store(0);
          encrypted_total_.store(0);
          encrypted_error_.store(0);
        }
        try {
          const std::wstring packed =
              std::to_wstring(encrypted_state_.load()) + L"," +
              std::to_wstring(encrypted_done_.load()) + L"," +
              std::to_wstring(encrypted_total_.load()) + L"," +
              std::to_wstring(encrypted_error_.load());
          ::VariantInit(result);
          return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
        } catch (...) { return E_FAIL; }
      }
      case kCancelEncryptedTransferId:
        if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
        encrypted_cancel_.store(true);
        return complete_selection(encrypted_state_.load() == 1, result);
      case kGetContainerCountId:
        return invoke_container_count(parameters, result);
      case kGetContainerIdId:
      case kGetContainerNameId:
      case kGetContainerMountedId:
      case kGetContainerSizeMibId:
      case kGetContainerPartitionId:
      case kGetContainerConversionId:
      case kGetContainerVolumeUnavailableId:
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
  // COM/window thread only. Both failure sources share one warning per mount.
  void warn_capacity_once(const std::uint32_t id, const bool protection) noexcept {
    if (!external_drop_sink_) return;
    try {
      const auto epoch = creation_work_->card_epoch.load();
      if (warning_epoch_ != epoch) {
        warned_capacity_ids_.clear();
        warning_epoch_ = epoch;
      }
      if (std::find(warned_capacity_ids_.begin(), warned_capacity_ids_.end(), id) != warned_capacity_ids_.end()) return;
      warned_capacity_ids_.push_back(id);
      external_drop_sink_->show_capacity_warning(protection);
    } catch (...) {} // Notification failure must not change protection facts.
  }

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
              USB_VAULT_TRACE_FAILURE("页面桥", "批量操作单项失败",
                                      "安全盘 #" + std::to_string(plan[i].target.id));
              outcome = volume_failure_status(error.code());
            } catch (...) { // One failed item does not stop an independent request.
              USB_VAULT_TRACE_FAILURE("页面桥", "批量操作单项失败",
                                      "安全盘 #" + std::to_string(plan[i].target.id));
            }
          }
          USB_VAULT_TRACE("页面桥", mount ? "批量挂载单项结果" : force ? "批量强制卸载单项结果" : "批量卸载单项结果",
                          "第 " + std::to_string(i + 1) + " / " + std::to_string(plan.size()) +
                              " 项；安全盘 #" + std::to_string(plan[i].target.id) + "；结果码 " +
                              std::to_string(interrupted() ? 10u : outcome));
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
          USB_VAULT_TRACE("页面桥", "批量操作结束",
                          "状态 " + std::to_string(batch_state_) + "；完成 " +
                              std::to_string(batch_completed_) + " / " + std::to_string(plan.size()) +
                              "；COM " + diagnostics::hex_text(static_cast<std::uint32_t>(com)));
          batch_current_ = 0;
          // Removed-card snapshots must not retain container names for a later login.
          if (interrupted()) batch_items_.clear();
        }
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      creation_work_->running.store(false);
      clear_batch();
      return E_FAIL;
    }
  }

  // Page-facing failure codes of an encrypted transfer. Role-neutral: none of
  // them depends on which container or which login the transfer touched.
  enum EncryptedFailure : unsigned int {
    encrypted_failure_none = 0,
    encrypted_failure_rejected = 1,
    encrypted_failure_mismatch = 2,
    encrypted_failure_weak = 3,
    encrypted_failure_exists = 4,
    encrypted_failure_space = 5,
    encrypted_failure_source = 6,
    encrypted_failure_other = 7,
    encrypted_failure_target = 8,
  };

  // Seven arguments: container, directory, entry, location, package name, and
  // the export password twice as byte arrays. Both arrays are consumed (and so
  // wiped) before anything else is looked at.
  HRESULT begin_encrypted_export(DISPPARAMS* const parameters, VARIANT* const result,
                                 UINT* const argument_error) noexcept {
    if (parameters == nullptr || parameters->cArgs != 7 ||
        parameters->cNamedArgs != 0 || parameters->rgvarg == nullptr) {
      wipe_available_arguments(parameters, parameters ? parameters->cArgs : 0);
      return DISP_E_BADPARAMCOUNT;
    }
    const DispatchTextWiper text_wiper(parameters);
    try {
      security::SecureBuffer password = consume_webview_password_array(parameters->rgvarg[1]);
      security::SecureBuffer confirmation = consume_webview_password_array(parameters->rgvarg[0]);
      std::uint32_t id = 0;
      std::wstring_view relative_directory, name, destination_directory, package_name;
      if (!exact_uint32(parameters->rgvarg[6], id) ||
          !exact_wstring(parameters->rgvarg[5], relative_directory) ||
          !exact_wstring(parameters->rgvarg[4], name) ||
          !exact_wstring(parameters->rgvarg[3], destination_directory) ||
          !exact_wstring(parameters->rgvarg[2], package_name) || password.empty() ||
          confirmation.empty()) {
        if (argument_error != nullptr) *argument_error = 0;
        return DISP_E_TYPEMISMATCH;
      }
      if (creation_work_->interrupted.load() ||
          controller_.state() != CardProductUiState::authenticated) {
        return E_ACCESSDENIED;
      }
      return begin_encrypted_transfer(
          [this, id, relative_directory = std::wstring(relative_directory),
           name = std::wstring(name), destination_directory = std::wstring(destination_directory),
           package_name = std::wstring(package_name), password = std::move(password),
           confirmation = std::move(confirmation)](
              const std::function<bool()>& cancelled,
              const std::function<void(std::uint64_t, std::uint64_t)>& progress) mutable {
            controller_.export_container_entry_encrypted(
                id, relative_directory, name, destination_directory, package_name,
                password.bytes(), confirmation.bytes(), cancelled, progress);
          },
          result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("页面桥", "加密传输未能开始", "");
      wipe_available_arguments(parameters, 2);
      return E_FAIL;
    }
  }

  [[nodiscard]] static unsigned int encrypted_failure(const std::exception_ptr& failure) noexcept {
    try {
      std::rethrow_exception(failure);
    } catch (const backup::ExportPackageRejected&) {
      return encrypted_failure_rejected;
    } catch (const security::PasswordConfirmationRejected& rejected) {
      return rejected.reason() == security::PasswordConfirmationRejection::mismatch
                 ? encrypted_failure_mismatch : encrypted_failure_weak;
    } catch (const std::system_error& error) {
      if (error.code().category() == std::system_category()) {
        switch (error.code().value()) {
          case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return encrypted_failure_exists;
          case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: case ERROR_FILE_TOO_LARGE:
            return encrypted_failure_space;
          default: break;
        }
      }
      return encrypted_failure_other;
    } catch (const std::invalid_argument&) {
      return encrypted_failure_target;
    } catch (const std::runtime_error&) {
      return encrypted_failure_source;
    } catch (...) {
      return encrypted_failure_other;
    }
  }

  [[nodiscard]] static bool cancelled_failure(const std::exception_ptr& failure) noexcept {
    try {
      std::rethrow_exception(failure);
    } catch (const std::system_error& error) {
      return error.code().category() == std::system_category() &&
             error.code().value() == ERROR_CANCELLED;
    } catch (...) {
      return false;
    }
  }

  // Runs one encrypted transfer on the session worker. State: 1 running,
  // 2 done, 3 failed (with a code), 4 cancelled. Card removal and session
  // revocation cancel it exactly as the page's cancel does.
  template <typename Operation>
  HRESULT begin_encrypted_transfer(Operation operation, VARIANT* const result) {
    if (creation_thread_.joinable()) creation_thread_.join();
    const auto epoch = creation_work_->card_epoch.load();
    encrypted_epoch_.store(epoch);
    encrypted_cancel_.store(false);
    encrypted_done_.store(0);
    encrypted_total_.store(0);
    encrypted_error_.store(encrypted_failure_none);
    encrypted_state_.store(1);
    creation_work_->finish();
    creation_work_->volume_operation.store(true);
    encrypted_mode_.store(true);
    creation_work_->running.store(true);
    try {
      creation_thread_ = std::thread([this, epoch, operation = std::move(operation)]() mutable {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto cancelled = [this, epoch] {
          return encrypted_cancel_.load() || creation_work_->interrupted.load() ||
                 creation_work_->card_epoch.load() != epoch;
        };
        const auto progress = [this](const std::uint64_t done, const std::uint64_t total) {
          encrypted_total_.store(total);
          encrypted_done_.store(done);
        };
        unsigned int state = 2;
        try {
          operation(cancelled, progress);
        } catch (...) {
          const auto failure = std::current_exception();
          USB_VAULT_TRACE_FAILURE("页面桥", "加密传输失败", "");
          if (cancelled_failure(failure)) {
            state = 4;
          } else {
            state = 3;
            encrypted_error_.store(encrypted_failure(failure));
          }
        }
        if (SUCCEEDED(com)) ::CoUninitialize();
        encrypted_state_.store(state);
        // Running ends first: until it does, the gate must still let this
        // transfer's own status and cancel through.
        creation_work_->running.store(false);
        encrypted_mode_.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("页面桥", "加密传输线程未能启动", "");
      encrypted_state_.store(3);
      encrypted_error_.store(encrypted_failure_other);
      encrypted_mode_.store(false);
      creation_work_->running.store(false);
      return E_FAIL;
    }
  }

  void clear_partitions() noexcept {
    std::lock_guard lock(partition_mutex_);
    partition_items_.clear();
    partition_selected_ = 0;
    partition_status_.store(0);
    partition_check_status_.store(0);
  }
  static HRESULT partition_number(std::uint32_t value, VARIANT* result) noexcept {
    if (!result) return E_POINTER;
    ::VariantInit(result); result->vt = VT_UI4; result->ulVal = value;
    return S_OK;
  }
  HRESULT invoke_partition(DISPID member, DISPPARAMS* parameters, VARIANT* result) noexcept {
    struct PairWiper {
      DISPPARAMS* args;
      ~PairWiper() { if (args) wipe_available_arguments(args, args->cArgs); }
    } pair_wiper{member == kPartitionPairId ? parameters : nullptr};
    if (member == kPartitionSelectId || member == kPartitionCreateId || member==kPartitionConvertId || member==kPartitionResumeId || member==kPartitionDisplayId || member==kPartitionPairId) {
      if (!parameters || parameters->cArgs != (member == kPartitionPairId ? 4u : 1u) || parameters->cNamedArgs || !parameters->rgvarg)
        return DISP_E_BADPARAMCOUNT;
    } else if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    if (!result) return E_POINTER;
    if ((member == kPartitionStatusId || member == kPartitionCheckStatusId) && creation_work_->running.load()) {
      return partition_number(1, result);
    }
    if (controller_.state() != CardProductUiState::authenticated || creation_work_->interrupted.load()) {
      // No worker touches controller_; snapshots are protected by this mutex.
      if (!creation_work_->running.load()) clear_partitions();
      return E_ACCESSDENIED;
    }
    try {
      if (member==kPartitionResumeId) {
        std::uint32_t id{};
        if(!exact_uint32(parameters->rgvarg[0],id) || !id)return DISP_E_TYPEMISMATCH;
        return begin_creation([this,id]{creation_paused_.store(controller_.resume_partition_conversion(id));},result,true);
      }
      if (member == kPartitionCreateId || member==kPartitionConvertId || member==kPartitionPairId) {
        if (!controller_.remembering_container_paths()) return E_ACCESSDENIED;
        std::uint32_t id{};
        if (!exact_uint32(parameters->rgvarg[member == kPartitionPairId ? 3 : 0], id) || !id) return DISP_E_TYPEMISMATCH;
        volume::PartitionCandidate target;
        const auto epoch = creation_work_->card_epoch.load();
        {
          std::lock_guard lock(partition_mutex_);
          if (partition_epoch_ != epoch || partition_status_.load() != 2 || partition_selected_ != id)
            return E_ACCESSDENIED;
          const auto found = std::ranges::find(partition_items_, id, &PartitionItem::id);
          if (found == partition_items_.end() || found->target.availability != volume::PartitionAvailability::candidate)
            return E_ACCESSDENIED;
          target = found->target;
        }
        std::uint32_t hidden_mib{};
        security::SecureBuffer second, confirmation;
        if (member == kPartitionPairId) {
          if (!exact_uint32(parameters->rgvarg[2], hidden_mib) || !hidden_mib ||
              static_cast<std::uint64_t>(hidden_mib) * 1048576 > target.length_bytes / 4 * 3)
            return DISP_E_TYPEMISMATCH;
          second = consume_webview_password_array(parameters->rgvarg[1]);
          confirmation = consume_webview_password_array(parameters->rgvarg[0]);
        }
        clear_partitions(); // Consume consent and snapshot; retries require a fresh selection.
        partition_mode_ = false;
        return begin_creation([this, target, epoch, member, hidden_mib, second=std::move(second), confirmation=std::move(confirmation)]() mutable {
          const auto confirm=[this, target, epoch](const volume::PartitionCandidate& confirmed) {
            return target == confirmed && !creation_work_->interrupted.load() && creation_work_->card_epoch.load() == epoch &&
                   !creation_work_->cancellation_requested();
          };
          prepare_bitlocker(target, epoch);
          if(member==kPartitionConvertId)creation_paused_.store(controller_.begin_partition_conversion(target,confirm));
          else if (member == kPartitionPairId)
            controller_.create_partition_pair(target, hidden_mib, second.bytes(), confirmation.bytes(), confirm);
          else controller_.create_partition(target,confirm);
        }, result,member==kPartitionConvertId);
      }
      if (member == kPartitionBeginId || member == kPartitionToolId) {
        last_volume_member_ = 0;
        clear_batch(); clear_survey(); clear_space();
      }
      if (member == kPartitionBeginId) {
        if (creation_thread_.joinable()) creation_thread_.join();
        clear_partitions();
        partition_epoch_ = creation_work_->card_epoch.load();
        const auto epoch = partition_epoch_;
        partition_mode_ = true;
        partition_status_.store(1);
        creation_work_->volume_operation.store(true);
        creation_work_->running.store(true);
        creation_thread_ = std::thread([this, epoch] {
          try {
            auto discovered = partition_services_.enumerate();
            std::lock_guard lock(partition_mutex_);
            if (creation_work_->interrupted.load() || creation_work_->card_epoch.load() != epoch) {
              partition_status_.store(4);
            } else if (discovered.size() > 4096 ||
                discovered.size() > (std::numeric_limits<std::uint32_t>::max)() - partition_next_id_) {
              partition_status_.store(3);
            } else {
              partition_items_.clear();
              for (auto& item : discovered) {
                if (item.availability == volume::PartitionAvailability::candidate && item.layout != volume::PartitionLayout::mbr && item.layout != volume::PartitionLayout::gpt)
                  item.availability = volume::PartitionAvailability::unsupported_layout;
                auto display = partition_services_.display_name(item);
                if (display.size() > 512) display.resize(512);
                partition_items_.push_back({++partition_next_id_, std::move(item), std::move(display)});
              }
              partition_status_.store(2);
            }
            USB_VAULT_TRACE("页面桥", "分区枚举结束",
                            "状态 " + std::to_string(partition_status_.load()) + "；分区数 " +
                                std::to_string(discovered.size()));
          } catch (...) {
            USB_VAULT_TRACE_FAILURE("页面桥", "分区枚举失败", "");
            std::lock_guard lock(partition_mutex_);
            partition_items_.clear(); partition_status_.store(3);
          }
          creation_work_->running.store(false);
        });
        return complete_selection(true, result);
      }
      if (member == kPartitionClearId || member == kPartitionToolId) {
        clear_partitions();
        if (member == kPartitionToolId && !partition_services_.open_disk_management()) return E_FAIL;
        return complete_selection(true, result);
      }
      if (member == kPartitionCheckId) {
        volume::PartitionCandidate target;
        const auto epoch = creation_work_->card_epoch.load();
        {
          std::lock_guard lock(partition_mutex_);
          const auto found = std::ranges::find(partition_items_, partition_selected_, &PartitionItem::id);
          if (partition_status_.load() != 2 || partition_epoch_ != epoch || !partition_selected_ || found == partition_items_.end()) return E_ACCESSDENIED;
          target = found->target;
        }
        if (creation_thread_.joinable()) creation_thread_.join();
        partition_mode_ = true;
        partition_check_status_.store(1);
        creation_work_->volume_operation.store(true);
        creation_work_->running.store(true);
        creation_thread_ = std::thread([this, epoch, target = std::move(target)] {
          unsigned int status = 3;
          try {
            if (!creation_work_->interrupted.load() && creation_work_->card_epoch.load() == epoch) {
              partition_services_.check(target);
              status = 2;
            }
          } catch (...) {
            USB_VAULT_TRACE_FAILURE("页面桥", "分区检查失败", "");
          }
          USB_VAULT_TRACE("页面桥", "分区检查结束", "状态 " + std::to_string(status));
          {
            std::lock_guard lock(partition_mutex_);
            if (creation_work_->interrupted.load() || creation_work_->card_epoch.load() != epoch) status = 4;
            if (status != 2) { partition_items_.clear(); partition_selected_ = 0; partition_status_.store(status); }
            partition_check_status_.store(status);
          }
          creation_work_->running.store(false);
        });
        return complete_selection(true, result);
      }
      std::lock_guard lock(partition_mutex_);
      if (partition_epoch_ != creation_work_->card_epoch.load()) return E_ACCESSDENIED;
      if (member == kPartitionStatusId) return partition_number(partition_status_.load(), result);
      if (member == kPartitionCheckStatusId) return partition_number(partition_check_status_.load(), result);
      if (partition_status_.load() != 2) return E_UNEXPECTED;
      if (member == kPartitionDisplayId) {
        std::uint32_t id{};
        if (!exact_uint32(parameters->rgvarg[0], id) || !id) return DISP_E_TYPEMISMATCH;
        const auto found = std::ranges::find(partition_items_, id, &PartitionItem::id);
        if (found == partition_items_.end()) return E_ACCESSDENIED;
        return complete_string(found->display.c_str(), static_cast<UINT>(found->display.size()), result);
      }
      if (member == kPartitionSelectedId) return partition_number(partition_selected_, result);
      if (member == kPartitionSelectId) {
        partition_selected_ = 0;
        partition_check_status_.store(0);
        std::uint32_t id{};
        if (!exact_uint32(parameters->rgvarg[0], id) || !id) return DISP_E_TYPEMISMATCH;
        const auto found = std::ranges::find(partition_items_, id, &PartitionItem::id);
        if (found == partition_items_.end() ||
            found->target.availability != volume::PartitionAvailability::candidate || found->target.query_error)
          return E_ACCESSDENIED;
        const auto current = partition_services_.enumerate();
        if (!volume::matches_partition_snapshot(found->target, current) ||
            creation_work_->interrupted.load() || partition_epoch_ != creation_work_->card_epoch.load() ||
            controller_.state() != CardProductUiState::authenticated) {
          partition_items_.clear();
          partition_status_.store(4);
          return E_ACCESSDENIED;
        }
        partition_selected_ = id;
        return complete_selection(true, result);
      }
      std::wstring packed;
      for (const auto& item : partition_items_) {
        const auto& target = item.target;
        if (!packed.empty()) packed += L"\n";
        packed += std::to_wstring(item.id) + L"," + std::to_wstring(target.disk_number) + L"," +
            std::to_wstring(target.partition_number) + L"," + std::to_wstring(target.length_bytes) + L"," +
            std::to_wstring(static_cast<unsigned int>(target.availability));
      }
      return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (member == kPartitionBeginId || member == kPartitionCheckId) creation_work_->running.store(false);
      clear_partitions();
      return E_FAIL;
    }
  }

  void clear_survey() noexcept {
    std::lock_guard lock(survey_mutex_);
    survey_found_.clear();
    survey_mode_ = false;
  }

  void clear_space() noexcept {
    std::lock_guard lock(space_mutex_);
    space_ready_ = false;
    space_id_ = 0;
  }

  // One argument: the decoy password, as the byte array every password takes.
  // It is wiped on every path, and what comes back is identifiers only.
  HRESULT begin_decoy_survey(DISPPARAMS* const parameters, VARIANT* const result,
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      wipe_available_arguments(parameters, 1);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      wipe_available_arguments(parameters, 1);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      wipe_available_arguments(parameters, 1);
      return E_FAIL;
    }

    try {
      if (creation_work_->interrupted.load() ||
          controller_.state() != CardProductUiState::authenticated) {
        return E_ACCESSDENIED;
      }
      if (creation_thread_.joinable()) creation_thread_.join();
      const auto epoch = creation_work_->card_epoch.load();
      {
        std::lock_guard lock(survey_mutex_);
        survey_found_.clear();
        survey_epoch_ = epoch;
        survey_state_ = 1;
        survey_total_ = 0;
        survey_done_ = 0;
        survey_unreachable_ = 0;
        survey_cancelled_ = false;
        survey_mode_ = true;
      }
      survey_cancel_.store(false);
      creation_work_->finish();
      creation_work_->volume_operation.store(true);
      volume_status_.store(3); // A walk is never a single-volume retry target.
      creation_work_->running.store(true);
      creation_thread_ = std::thread([this, password = std::move(password), epoch]() mutable {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto interrupted = [this, epoch] {
          return creation_work_->interrupted.load() ||
                 creation_work_->card_epoch.load() != epoch;
        };
        unsigned int state = 3;
        try {
          if (FAILED(com)) throw std::runtime_error("worker COM initialization failed: HRESULT " + diagnostics::hex_text(static_cast<std::uint32_t>(com)));
          const auto survey = controller_.survey_decoy_maintenance(
              password.bytes(),
              [this](const std::uint32_t done, const std::uint32_t total) {
                std::lock_guard lock(survey_mutex_);
                survey_done_ = done;
                survey_total_ = total;
              },
              [this, &interrupted] {
                return survey_cancel_.load() || interrupted();
              });
          std::lock_guard lock(survey_mutex_);
          survey_found_ = survey.maintainable;
          survey_unreachable_ = survey.unreachable;
          survey_cancelled_ = survey.cancelled;
          state = 2;
        } catch (...) {
          // Every refusal the session makes is one refusal, and so is this.
          USB_VAULT_TRACE_FAILURE("页面桥", "诱饵搜索失败", "");
        }
        USB_VAULT_TRACE("页面桥", "诱饵搜索结束", "状态 " + std::to_string(state));
        password.clear();
        if (SUCCEEDED(com)) ::CoUninitialize();
        {
          std::lock_guard lock(survey_mutex_);
          survey_state_ = interrupted() ? 4U : state;
          // A removed card must not leave a list of containers behind.
          if (interrupted()) survey_found_.clear();
        }
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      password.clear();
      creation_work_->running.store(false);
      clear_survey();
      return E_FAIL;
    }
  }

  HRESULT invoke_survey_query(DISPID member, DISPPARAMS* parameters, VARIANT* result) noexcept {
    const bool by_index = member == kDecoySurveyIdId;
    if (!parameters || parameters->cNamedArgs != 0 ||
        parameters->cArgs != (by_index ? 1U : 0U) ||
        (by_index && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
    std::uint32_t index = 0;
    if (by_index && !exact_uint32(parameters->rgvarg[0], index)) return DISP_E_TYPEMISMATCH;
    if (!result) return E_POINTER;
    try {
      if (!survey_mode_) return E_UNEXPECTED;
      if (survey_epoch_ != creation_work_->card_epoch.load() ||
          creation_work_->interrupted.load() ||
          (!creation_work_->running.load() &&
           controller_.state() != CardProductUiState::authenticated)) {
        if (!creation_work_->running.load()) clear_survey();
        return E_ACCESSDENIED;
      }
      if (member == kCancelDecoySurveyId) {
        survey_cancel_.store(true);
        return complete_selection(true, result);
      }
      std::lock_guard lock(survey_mutex_);
      if (by_index) {
        if (index >= survey_found_.size()) return E_INVALIDARG;
        ::VariantInit(result);
        result->vt = VT_UI4;
        result->ulVal = survey_found_[index];
        return S_OK;
      }
      const std::wstring packed = std::to_wstring(survey_state_) + L"," +
          std::to_wstring(survey_total_) + L"," + std::to_wstring(survey_done_) + L"," +
          std::to_wstring(survey_found_.size()) + L"," +
          std::to_wstring(survey_unreachable_) + L"," + (survey_cancelled_ ? L"1" : L"0");
      return complete_string(packed.c_str(), static_cast<UINT>(packed.size()), result);
    } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
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
    } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__)); return E_FAIL; }
  }

  HRESULT begin_volume_operation(DISPID member, DISPPARAMS* parameters,
                                 VARIANT* result, UINT* argument_error) noexcept {
    // Keep the retired member fail-closed for stale pages. Forced application
    // exit is owned by the native hotkey path, never by page confirmation.
    if (member == kBeginForceUnmountForExitId) return E_ACCESSDENIED;
    const bool force = member == kBeginForceUnmountLastId;
    if (force) {
      if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
      if (volume_status_.load() != 4 || last_volume_epoch_ != creation_work_->card_epoch.load() ||
          (last_volume_member_ != kBeginUnmountId && last_volume_member_ != kBeginUnmountContainerId))
        return E_ACCESSDENIED;
      member = last_volume_member_;
    }
    // Unmounting everything is the only call without an identifier, and the
    // cleanup exemption below: it stays reachable once the card is gone.
    const bool all = member == kBeginUnmountId;
    if (!parameters || parameters->cNamedArgs != 0 ||
        parameters->cArgs != (all || force ? 0U : 1U) ||
        (!all && !force && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
    std::uint32_t id = force ? last_volume_id_ : 0;
    if (!all && !force && (!exact_uint32(parameters->rgvarg[0], id) || id == 0)) {
      if (argument_error) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    try {
      // Cleanup remains available in the unavailable state. A removed card
      // must be processed by the window before any new worker is admitted.
      if (creation_work_->interrupted.load()) return E_PENDING;
      if (!all && controller_.state() != CardProductUiState::authenticated) return E_ACCESSDENIED;
      if (creation_thread_.joinable()) creation_thread_.join();
      // Completed batches and surveys retain results for their callers. Once
      // the next volume operation is admitted, retire both modes so its own
      // progress reader is allowed while unrelated session reads stay blocked.
      clear_batch();
      clear_survey();
      if (member == kBeginOpenDecoyMaintenanceId ||
          member == kBeginMeasureDecoySpaceId ||
          member == kBeginEndDecoyMaintenanceId) {
        clear_space();
      }
      last_volume_member_ = member;
      if (member == kBeginOpenDecoyMaintenanceId) {
        std::erase(warned_capacity_ids_, id);
        // Even an unpolled failure belongs to the mount being retired.
        if (external_drop_sink_) warned_import_run_ = external_drop_sink_->status().run_id;
      }
      last_volume_id_ = id;
      last_volume_epoch_ = creation_work_->card_epoch.load();
      const std::uint64_t operation_epoch = last_volume_epoch_;
      creation_work_->finish(); // Volume operations cannot be cancelled.
      creation_work_->volume_operation.store(true);
      volume_status_.store(1);
      creation_work_->running.store(true);
      creation_thread_ = std::thread([this, member, id, force,
                                      operation_epoch] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        unsigned int status = 3;
        try {
          if (FAILED(com)) throw std::runtime_error("worker COM initialization failed: HRESULT " + diagnostics::hex_text(static_cast<std::uint32_t>(com)));
          if (!creation_work_->interrupted.load()) {
            if (force && member == kBeginUnmountContainerId) controller_.force_unmount_container(id);
            else if (force) controller_.force_unmount_user_active();
            else if (member == kBeginOpenDecoyMaintenanceId)
              static_cast<void>(controller_.open_decoy_maintenance(id));
            else if (member == kBeginMeasureDecoySpaceId) {
              const auto space = controller_.decoy_maintenance_space(id);
              std::lock_guard lock(space_mutex_);
              space_total_ = space.total_bytes;
              space_free_ = space.free_bytes;
              space_safe_free_ = space.safe_free_bytes;
              space_id_ = id;
              space_epoch_ = operation_epoch;
              space_ready_ = true;
            }
            else if (member == kBeginEndDecoyMaintenanceId) {
              controller_.end_decoy_maintenance(id);
              std::lock_guard lock(space_mutex_);
              space_ready_ = false;
            }
            else if (member == kBeginMountAdoptedId) controller_.mount_container(id);
            else if (member == kBeginUnmountContainerId) controller_.unmount_container(id);
            else controller_.unmount();
            status = 2;
          }
        } catch (const std::system_error& error) {
          USB_VAULT_TRACE_FAILURE("页面桥", "卷操作失败", member_text(member) + "；安全盘 #" + std::to_string(id));
          status = volume_failure_status(error.code());
        } catch (...) {
          USB_VAULT_TRACE_FAILURE("页面桥", "卷操作失败", member_text(member) + "；安全盘 #" + std::to_string(id));
        }
        if (SUCCEEDED(com)) ::CoUninitialize();
        // A removal during an IOCTL is handled by the deferred lifecycle path.
        // Do not present its late result as a successful user operation.
        if (creation_work_->interrupted.load()) {
          status = 10;
          clear_space();
        }
        USB_VAULT_TRACE("页面桥", "卷操作结束",
                        member_text(member) + "；安全盘 #" + std::to_string(id) + "；强制 " +
                            diagnostics::flag_text(force) + "；状态码 " + std::to_string(status) +
                            "；安全卡中断 " + diagnostics::flag_text(creation_work_->interrupted.load()));
        volume_status_.store(status);
        creation_work_->force_exit_running.store(false);
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      volume_status_.store(3);
      creation_work_->force_exit_running.store(false);
      creation_work_->running.store(false);
      return E_FAIL;
    }
  }

  void prepare_bitlocker(const volume::PartitionCandidate& target, std::uint64_t epoch) {
    const auto cancelled = [this, epoch] {
      return creation_work_->cancellation_requested() || creation_work_->interrupted.load() ||
             creation_work_->card_epoch.load() != epoch;
    };
    const auto guard = [&] {
      if (cancelled()) throw std::system_error(volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled));
    };
    const auto inspect = [&] {
      guard();
      const auto status = partition_services_.inspect_bitlocker(target);
      guard();
      if (!volume::valid_bitlocker_status(status)) throw std::runtime_error("invalid BitLocker status");
      bitlocker_status_.store((status.state << 8) | status.encrypted_percent);
      return status;
    };
    if (inspect().state == 0) return;
    bitlocker_phase_.store(1);
    while (!bitlocker_consent_.load()) { guard(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    guard();
    bitlocker_phase_.store(2);
    partition_services_.decrypt_bitlocker(target, [&] { return bitlocker_consent_.load() && !cancelled(); }, cancelled);
    while (inspect().state != 0) {
      for (unsigned int i = 0; i < 20; ++i) { guard(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    guard();
    bitlocker_consent_.store(false);
    bitlocker_phase_.store(0);
  }

  template <typename Operation>
  HRESULT begin_creation(Operation operation, VARIANT* result, bool inplace = false) {
    if (creation_work_->interrupted.load() || controller_.state() != CardProductUiState::authenticated) return E_FAIL;
    if (creation_thread_.joinable()) creation_thread_.join();
    creation_progress_.store(0);
    creation_flags_.store(0);
    creation_error_.store(0);
    bitlocker_phase_.store(0);
    bitlocker_status_.store(0);
    bitlocker_consent_.store(false);
    bitlocker_epoch_.store(creation_work_->card_epoch.load());
    creation_status_.store(1);
    creation_paused_.store(false);
    creation_work_->inplace_operation.store(inplace);
    creation_work_->volume_operation.store(false);
    creation_work_->begin();
    creation_work_->running.store(true);
    try {
      creation_thread_ = std::thread([this, operation = std::move(operation)]() mutable {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        unsigned int status = 3;
        try {
          if (FAILED(com)) throw std::runtime_error("worker COM initialization failed: HRESULT " + diagnostics::hex_text(static_cast<std::uint32_t>(com)));
          controller_.set_creation_revocation([this,epoch=creation_work_->card_epoch.load()]{
            return creation_work_->interrupted.load() || creation_work_->card_epoch.load()!=epoch;
          });
          controller_.set_creation_cancellation(
              [this, epoch = creation_work_->card_epoch.load()]() {
                return creation_work_->cancellation_requested() || creation_work_->interrupted.load() ||
                       creation_work_->card_epoch.load() != epoch;
              });
          controller_.set_creation_feedback([this](unsigned int stage, unsigned int percent, wchar_t drive) {
            if (stage == 4 && creation_work_->interrupted.load()) throw std::runtime_error("card removed during creation");
            // Stage 4 closes the cancel gate before mounting. File creation
            // emits it before publishing, inside its staging-file cleanup
            // scope, and aborts if a request won the race. Partition creation
            // emits it after the write and ignores that late request.
            if (stage == 4 && !creation_work_->begin_finalization()) {
              throw std::system_error(volume_creation::make_error_code(
                  volume_creation::CreationError::operation_cancelled));
            }
            if (stage == 1 || stage == 2) creation_flags_.fetch_or(1U << (stage - 1));
            creation_progress_.store((static_cast<std::uint64_t>(stage) << 32) |
                (static_cast<std::uint64_t>(percent) << 16) | static_cast<unsigned int>(drive));
          });
          operation();
          status = creation_paused_.load() ? 6u : 2u;
        } catch (const application::ContainerCreationCleanupError&) {
          USB_VAULT_TRACE_FAILURE("页面桥", "创建后台任务清理失败", "");
          status = 5;
        } catch (const std::system_error& error) {
          USB_VAULT_TRACE_FAILURE("页面桥", "创建后台任务失败", "");
          creation_error_.store(creation_failure_code(error.code()));
          // A request alone proves neither cancellation nor successful cleanup.
          // Only the creator's confirmed outcome may claim cancellation.
          if (error.code() == volume_creation::make_error_code(
                  volume_creation::CreationError::operation_cancelled)) {
            status = 4;
          } else if (error.code() == volume_creation::make_error_code(
                         volume_creation::CreationError::cleanup_failed)) {
            status = 5;
          }
        } catch (...) {
          USB_VAULT_TRACE_FAILURE("页面桥", "创建后台任务失败", "");
        }
        try { controller_.set_creation_feedback({}); } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));}
        try { controller_.set_creation_cancellation({}); } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));}
        try { controller_.set_creation_revocation({}); } catch (...) { USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));}
        if (SUCCEEDED(com)) ::CoUninitialize();
        if (status >= 3 && status <= 5 && !creation_error_.load()) creation_error_.store(static_cast<unsigned int>(E_FAIL));
        USB_VAULT_TRACE("页面桥", "创建后台任务结束",
                        "状态 " + std::to_string(status) + "；页面错误码 " +
                            std::to_string(creation_error_.load()) + "（" +
                            diagnostics::hex_text(creation_error_.load()) + "）；原地加密 " +
                            diagnostics::flag_text(creation_work_->inplace_operation.load()) + "；已暂停 " +
                            diagnostics::flag_text(creation_paused_.load()));
        creation_status_.store(status);
        creation_work_->finish();
        creation_work_->running.store(false);
      });
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      creation_error_.store(static_cast<unsigned int>(E_FAIL));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      wipe_available_arguments(parameters, 6);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      wipe_available_arguments(parameters, 6);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  HRESULT invoke_mount_container(DISPPARAMS* const parameters,
                                 VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      return complete_selection(controller_.mount_container(), result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  // Two arguments: the container identifier and the directory inside it. COM
  // delivers them in reverse order, as it does everywhere here.
  void record_directory_failure(const DirectoryFailure reason) noexcept {
    directory_error_.store(static_cast<unsigned int>(reason));
  }

  // Whether the drive carrying this container still answers. A not-found
  // result means two different things to a person -- the folder was deleted,
  // or the drive it lived on is no longer there -- and only the volume root
  // separates them.
  [[nodiscard]] bool volume_root_reachable(const std::uint32_t id) noexcept {
    try {
      for (const ContainerUiStatus& status : controller_.container_statuses()) {
        if (status.id != id) continue;
        if (status.drive_letter < L'A' || status.drive_letter > L'Z') return false;
        const wchar_t root[] = {status.drive_letter, L':', L'\\', L'\0'};
        const DWORD attributes = ::GetFileAttributesW(root);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      }
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    }
    return false;
  }

  [[nodiscard]] DirectoryFailure classify_listing_error(
      const std::uint32_t id, const std::system_error& error) noexcept {
    const std::error_code code = error.code();
    const int value = code.value();
    // Matched on the Win32 value alone. The generic mapping folds unrelated
    // conditions such as a write-protected device into permission_denied, and
    // telling someone they lack permission to read is wrong in those cases.
    if (value == static_cast<int>(ERROR_ACCESS_DENIED) &&
        code.category() == std::system_category()) {
      return DirectoryFailure::access_denied;
    }
    if (value == static_cast<int>(ERROR_NOT_READY) ||
        value == static_cast<int>(ERROR_INVALID_DRIVE) ||
        value == static_cast<int>(ERROR_DEV_NOT_EXIST) ||
        value == static_cast<int>(ERROR_UNRECOGNIZED_VOLUME)) {
      return DirectoryFailure::volume_unreachable;
    }
    if (code != std::errc::no_such_file_or_directory &&
        value != static_cast<int>(ERROR_FILE_NOT_FOUND) &&
        value != static_cast<int>(ERROR_PATH_NOT_FOUND)) {
      return DirectoryFailure::io_error;
    }
    return volume_root_reachable(id) ? DirectoryFailure::directory_missing
                                     : DirectoryFailure::volume_unreachable;
  }

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
    directory_error_.store(static_cast<unsigned int>(DirectoryFailure::none));
    try {
      packed = pack_directory_listing(
          controller_.list_container_directory(id, relative_directory));
    } catch (const std::invalid_argument&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      // The page only ever sends an adopted identifier and a directory it was
      // given, so in practice this is a container the session no longer holds.
      record_directory_failure(DirectoryFailure::unknown_container);
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::logic_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      record_directory_failure(DirectoryFailure::not_mounted);
      return E_FAIL;
    } catch (const std::system_error& error) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      record_directory_failure(classify_listing_error(id, error));
      return E_FAIL;
    } catch (const std::runtime_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      // The containment gate and the entry-name check both land here: the
      // volume answered, and what it answered with was refused.
      record_directory_failure(DirectoryFailure::contents_refused);
      return E_FAIL;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      record_directory_failure(DirectoryFailure::unknown);
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  HRESULT invoke_rename_container(DISPPARAMS* const parameters,
                                  UINT* const argument_error) noexcept {
    if (!parameters || parameters->cArgs != 2 || parameters->cNamedArgs != 0 || !parameters->rgvarg)
      return DISP_E_BADPARAMCOUNT;
    const DispatchTextWiper text_wiper(parameters);
    std::uint32_t id{};
    std::wstring_view name;
    if (!exact_uint32(parameters->rgvarg[1], id)) {
      if (argument_error) *argument_error = 1;
      return DISP_E_TYPEMISMATCH;
    }
    if (!exact_wstring(parameters->rgvarg[0], name)) {
      if (argument_error) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    }
    if (!id || !application::valid_container_index_name(name)) return E_INVALIDARG;
    if (creation_work_->interrupted.load()) return E_ACCESSDENIED;
    try {
      controller_.rename_container(id, name);
      return S_OK;
    } catch (const std::invalid_argument&) {
      return E_INVALIDARG;
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
        case kGetContainerConversionId:
          result->vt=VT_UI4; result->ulVal=status.conversion_state; return S_OK;
        case kGetContainerPartitionId:
          result->vt = VT_BOOL;
          result->boolVal = status.partition ? VARIANT_TRUE : VARIANT_FALSE;
          return S_OK;
        case kGetContainerVolumeUnavailableId:
          result->vt = VT_BOOL;
          result->boolVal =
              status.volume_unavailable ? VARIANT_TRUE : VARIANT_FALSE;
          return S_OK;
        case kGetContainerDriveLetterId:
        default: {
          const wchar_t drive[] = {status.drive_letter, L':'};
          return complete_string(
              drive, status.drive_letter == wchar_t{} ? 0U : 2U, result);
        }
      }
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  HRESULT invoke_login_reset(DISPID member, DISPPARAMS* parameters, VARIANT* result) noexcept {
    const bool confirming = member == kBeginConfirmLoginResetId;
    const bool cancelling = member == kCancelLoginResetId;
    if (!parameters || parameters->cNamedArgs != 0 ||
        parameters->cArgs != (confirming ? 2U : cancelling ? 1U : 0U) ||
        ((confirming || cancelling) && !parameters->rgvarg)) return DISP_E_BADPARAMCOUNT;
    if (!result) return E_POINTER;
    std::uint32_t ticket = 0;
    bool force = false;
    if ((confirming || cancelling) && !exact_uint32(parameters->rgvarg[confirming ? 1 : 0], ticket))
      return DISP_E_TYPEMISMATCH;
    if (confirming) {
      const auto& choice = parameters->rgvarg[0];
      if (choice.vt != VT_BOOL || (choice.boolVal != VARIANT_TRUE && choice.boolVal != VARIANT_FALSE))
        return DISP_E_TYPEMISMATCH;
      force = choice.boolVal == VARIANT_TRUE;
    }
    try {
      if (member == kLoginResetStatusId && reset_mode_ && creation_work_->running.load()) {
        ::VariantInit(result); result->vt = VT_UI4; result->ulVal = 1; return S_OK;
      }
      if (creation_work_->running.load()) return E_PENDING;
      if (creation_thread_.joinable()) creation_thread_.join();
      if (member == kCanLoginResetId) return complete_selection(controller_.can_login_reset(), result);
      if (member == kBeginLoginResetPreparationId) {
        if (creation_work_->interrupted.load() || !controller_.can_login_reset()) return E_ACCESSDENIED;
        reset_result_ = {}; reset_epoch_ = creation_work_->card_epoch.load();
      } else {
        if (!reset_mode_ || creation_work_->interrupted.load() || reset_epoch_ != creation_work_->card_epoch.load()) {
          controller_.cancel_login_reset(); reset_result_ = {}; reset_status_.store(6);
          if (member != kLoginResetStatusId) return E_ACCESSDENIED;
        }
        if (member == kLoginResetStatusId) {
          auto status = reset_status_.load();
          if ((status == 2 || status == 4) && !controller_.login_reset_pending(reset_result_.ticket)) {
            controller_.cancel_login_reset(); reset_result_ = {}; status = 6; reset_status_.store(status);
          }
          ::VariantInit(result); result->vt = VT_UI4; result->ulVal = status;
          if (status == 5 && !reset_close_requested_) {
            reset_close_requested_ = true;
            if (close_sink_) close_sink_->request_close();
          }
          return S_OK;
        }
        if (!controller_.login_reset_pending(reset_result_.ticket)) return E_ACCESSDENIED;
        if (confirming || cancelling) {
          if (ticket != reset_result_.ticket) { controller_.cancel_login_reset(); return E_ACCESSDENIED; }
          if (cancelling) { controller_.cancel_login_reset(); reset_result_ = {}; reset_status_.store(0); return complete_selection(true, result); }
        } else if (member == kLoginResetDrivesId) {
          return complete_string(reset_result_.drives.c_str(), static_cast<UINT>(reset_result_.drives.size()), result);
        } else if (member == kLoginResetMountsKnownId) {
          return complete_selection(reset_result_.mounts_known, result);
        } else if (member == kLoginResetTicketId) {
          ::VariantInit(result); result->vt = VT_UI4; result->ulVal = reset_result_.ticket; return S_OK;
        }
      }
      clear_survey(); clear_batch();
      reset_mode_ = true; reset_close_requested_ = false;
      creation_work_->finish(); creation_work_->volume_operation.store(true);
      reset_status_.store(1); creation_work_->running.store(true);
      const auto epoch = reset_epoch_;
      creation_thread_ = std::thread([this, confirming, ticket, force, epoch] {
        unsigned int status = 3;
        const auto interrupted = [this, epoch] {
          return creation_work_->interrupted.load() || creation_work_->card_epoch.load() != epoch;
        };
        try {
          if (interrupted()) throw std::runtime_error("reset interrupted");
          if (!confirming) {
            reset_result_ = controller_.prepare_login_reset(); status = 2;
          } else {
            if (force) {
              try { controller_.force_login_reset_mounts(ticket, interrupted); }
              catch (...) {
                USB_VAULT_TRACE_FAILURE("页面桥", "重置前强制卸载失败", "");
                status = 4;
              }
            }
            if (status != 4) {
              if (interrupted()) throw std::runtime_error("reset interrupted");
              controller_.confirm_login_reset(ticket); status = 5;
            }
          }
        } catch (...) {
          USB_VAULT_TRACE_FAILURE("页面桥", "登录重置失败", confirming ? "确认阶段" : "准备阶段");
          controller_.cancel_login_reset();
        }
        if (interrupted()) { controller_.cancel_login_reset(); status = 6; }
        USB_VAULT_TRACE("页面桥", "登录重置后台任务结束",
                        std::string(confirming ? "确认阶段" : "准备阶段") + "；状态 " + std::to_string(status) +
                            "；强制 " + diagnostics::flag_text(force));
        reset_status_.store(status);
        creation_work_->running.store(false);
      });
      return complete_selection(true, result);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      creation_work_->running.store(false); controller_.cancel_login_reset(); reset_status_.store(3); return E_FAIL;
    }
  }

  HRESULT invoke_can_reset_card_record(DISPPARAMS* const parameters,
                                      VARIANT* const result) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    bool available = false;
    try {
      available = controller_.can_reset_card_record();
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
    return complete_selection(available, result);
  }

  HRESULT invoke_reset_card_record(DISPPARAMS* const parameters) noexcept {
    if (!has_no_arguments(parameters)) return DISP_E_BADPARAMCOUNT;
    try {
      controller_.reset_card_record();
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
    try {
      controller_.submit_login(password.bytes());
      return S_OK;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
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
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 1;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 1;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }

    security::SecureBuffer confirmation;
    try {
      confirmation =
          consume_webview_password_array(confirmation_argument);
    } catch (const std::invalid_argument&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (const std::length_error&) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      if (argument_error != nullptr) *argument_error = 0;
      return DISP_E_TYPEMISMATCH;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }

    try {
      controller_.submit_initialization(password.bytes(),
                                        confirmation.bytes());
      return S_OK;
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("ui/card_product_webview_host_object", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      return E_FAIL;
    }
  }

  struct PartitionItem { std::uint32_t id; volume::PartitionCandidate target; std::wstring display; };
  IUiPartitionServices& partition_services_;
  std::mutex partition_mutex_;
  std::vector<PartitionItem> partition_items_;
  std::atomic<unsigned int> partition_status_{0}; // 0 idle, 1 scanning, 2 ready, 3 failed, 4 expired.
  std::atomic<unsigned int> partition_check_status_{0}; // 0 idle, 1 checking, 2 verified, 3 failed, 4 expired.
  bool partition_mode_{}; // COM thread only; only this worker may expose its progress.
  std::uint64_t partition_epoch_{};
  std::uint32_t partition_next_id_{}; // Never reused by this bridge after refresh.
  std::uint32_t partition_selected_{};
  std::atomic<ULONG> references_{1};
  CardProductUiSessionController& controller_;
  IUiCloseRequestSink* close_sink_{};
  bool reset_mode_{};
  bool reset_close_requested_{};
  std::atomic<unsigned int> reset_status_{};
  std::uint64_t reset_epoch_{};
  LoginResetPreparation reset_result_;
  bool configuration_pending_{};
  bool backup_pending_{};
  IUiWindowSizeSink* window_size_sink_{};
  IUiExitHotkeySink* exit_hotkey_sink_{};
  IUiExternalDropTargetSink* external_drop_sink_{};
  std::vector<std::uint32_t> warned_capacity_ids_;
  std::uint64_t warning_epoch_{};
  std::uint64_t warned_import_run_{};
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
  std::mutex space_mutex_;
  std::uint64_t space_total_{};
  std::uint64_t space_free_{};
  std::uint64_t space_safe_free_{};
  std::uint32_t space_id_{};
  std::uint64_t space_epoch_{};
  bool space_ready_{false};
  bool survey_mode_{false}; // Accessed only by the COM/window thread.
  std::atomic<bool> encrypted_mode_{false};  // Cleared by the worker as it ends.
  std::atomic<bool> encrypted_cancel_{false};
  std::atomic<unsigned int> encrypted_state_{0};
  std::atomic<std::uint64_t> encrypted_done_{0};
  std::atomic<std::uint64_t> encrypted_total_{0};
  std::atomic<unsigned int> encrypted_error_{0};
  std::atomic<std::uint64_t> encrypted_epoch_{0};
  std::mutex survey_mutex_;
  std::vector<std::uint32_t> survey_found_;
  std::uint64_t survey_epoch_{};
  unsigned int survey_state_{};
  std::uint32_t survey_total_{};
  std::uint32_t survey_done_{};
  std::uint32_t survey_unreachable_{};
  bool survey_cancelled_{};
  std::atomic<bool> survey_cancel_{false};
  DISPID last_volume_member_{0};
  std::uint32_t last_volume_id_{0};
  std::uint64_t last_volume_epoch_{0};
  std::atomic<unsigned int> bitlocker_phase_{0};
  std::atomic<unsigned int> bitlocker_status_{0};
  std::atomic<bool> bitlocker_consent_{false};
  std::atomic<std::uint64_t> bitlocker_epoch_{0};
  std::atomic<unsigned int> creation_error_{0};
  std::atomic<unsigned int> directory_error_{
      static_cast<unsigned int>(DirectoryFailure::none)};
  std::atomic<unsigned int> creation_status_{0};
  std::atomic<bool> creation_paused_{false};
  std::atomic<std::uint64_t> creation_progress_{0};
  std::atomic<unsigned int> creation_flags_{0};
};

}  // namespace

bool resume_card_backup_webview_host_object(IDispatch* object) noexcept {
  auto* host = dynamic_cast<CardProductWebViewHostObject*>(object);
  return host && host->resume_card_backup();
}

HRESULT create_card_product_webview_host_object(
    CardProductUiSessionController& controller,
    IUiCloseRequestSink* const close_sink,
    IDispatch** const result,
    IUiWindowSizeSink* const window_size_sink,
    CreationWorkState* const creation_work,
    IUiExternalDropTargetSink* const external_drop_sink,
      IUiPartitionServices* const partition_services,
    IUiExitHotkeySink* const exit_hotkey_sink) noexcept {
  if (result == nullptr) return E_POINTER;
  *result = nullptr;
  auto* const object =
      new (std::nothrow) CardProductWebViewHostObject(controller, close_sink,
          window_size_sink, creation_work, external_drop_sink, partition_services, exit_hotkey_sink);
  if (object == nullptr) return E_OUTOFMEMORY;
  *result = object;
  return S_OK;
}

}  // namespace usb_vault::ui
