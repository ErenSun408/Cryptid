#include "card/diagnostic_error.h"
#include "card/vikey/card_operation_lease.h"
#include "card/process_security_failure.h"
#include "card/capture_failure.h"
#include "card/vikey/vikey_helper_client.h"

#include "card/vikey/vikey_helper_protocol.h"
#include "usb_vault/card/card_error.h"

#include <Windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::card::vikey {
namespace {

constexpr std::uint64_t kMaximumHelperBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumVendorBytes = 16ULL * 1024ULL * 1024ULL;
constexpr wchar_t kVendorDllName[] = L"ViKey.dll";
constexpr wchar_t kDevelopmentDirectoryPrefix[] =
    L"usb-vault-vikey-development-helper-";

enum class HelperBoundary {
  zero_network_app_container,
  network_enabled_development,
};

struct DevelopmentSessionInputs final {
  const security::SecureBuffer& user_sn;
  const security::SecureBuffer& api_key;
  const security::SecureBuffer& user_pin;
  const security::SecureBuffer* blob{};
  std::uint32_t expected_hardware_id{};
  helper::CardMediaState expected_media_state{};
};

constexpr DWORD64 kHelperMitigations =
    PROCESS_CREATION_MITIGATION_POLICY_DEP_ENABLE |
    PROCESS_CREATION_MITIGATION_POLICY_SEHOP_ENABLE |
    PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
    PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;

class Handle final {
 public:
  Handle() noexcept = default;
  explicit Handle(const HANDLE value) noexcept : value_(value) {}
  ~Handle() noexcept { reset(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  void reset() noexcept {
    if (*this) {
      static_cast<void>(::CloseHandle(value_));
    }
    value_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE value_{INVALID_HANDLE_VALUE};
};

class LocalSid final {
 public:
  LocalSid() noexcept = default;
  explicit LocalSid(PSID value) noexcept : value_(value) {}
  ~LocalSid() noexcept { reset(); }

  LocalSid(const LocalSid&) = delete;
  LocalSid& operator=(const LocalSid&) = delete;
  LocalSid(LocalSid&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  LocalSid& operator=(LocalSid&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] PSID get() const noexcept { return value_; }

 private:
  void reset() noexcept {
    if (value_ != nullptr) {
      static_cast<void>(::FreeSid(value_));
      value_ = nullptr;
    }
  }

  PSID value_{};
};

struct Pipe final {
  Handle parent_end;
  Handle child_end;
};

[[noreturn]] void throw_process_security_win32(const std::uint32_t step,
    const char* const context) {
  const DWORD error = ::GetLastError();
  throw_process_security(step, context, error, NativeCodeKind::win32);
}

// Report only a fixed missing-property step, never actual token/property values.
void require_security_properties(const std::uint32_t actual,
    const std::uint32_t required,
    std::initializer_list<std::pair<helper::SecurityProperty, std::uint32_t>> steps) {
  for (const auto& [property, step] : steps) {
    const auto bit = helper::property_bit(property);
    if ((required & bit) != 0 && (actual & bit) == 0)
      throw_process_security(step, "helper required security property missing");
  }
}

class AttributeList final {
 public:
  explicit AttributeList(const DWORD attribute_count) {
    SIZE_T bytes = 0;
    static_cast<void>(
        ::InitializeProcThreadAttributeList(nullptr, attribute_count, 0,
                                            &bytes));
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_process_security_win32(1, "size process attributes");
    }
    if (bytes == 0) throw_process_security(89, "zero attribute size");
    storage_.resize(bytes);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (::InitializeProcThreadAttributeList(list_, attribute_count, 0,
                                            &bytes) == FALSE) {
      throw_process_security_win32(2, "initialize process attributes");
    }
  }

  ~AttributeList() noexcept {
    if (list_ != nullptr) {
      ::DeleteProcThreadAttributeList(list_);
    }
  }

  AttributeList(const AttributeList&) = delete;
  AttributeList& operator=(const AttributeList&) = delete;

  void add(const DWORD_PTR attribute, void* const value,
           const SIZE_T bytes, const std::uint32_t step) {
    if (::UpdateProcThreadAttribute(list_, 0, attribute, value, bytes,
                                    nullptr, nullptr) == FALSE) {
      throw_process_security_win32(step, "set process attribute");
    }
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
    return list_;
  }

 private:
  std::vector<std::byte> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_{};
};

[[noreturn]] void throw_card(const CardError error, const char* const context,
    const DiagnosticStage stage = DiagnosticStage::none) {
  throw DiagnosticError(make_error_code(error), stage, context);
}

[[nodiscard]] Pipe create_pipe(const bool child_reads) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read = INVALID_HANDLE_VALUE;
  HANDLE write = INVALID_HANDLE_VALUE;
  if (::CreatePipe(&read, &write, &security, 0) == FALSE) {
    throw_card(CardError::backend_unavailable, "create ViKey helper IPC pipe", DiagnosticStage::ipc_setup);
  }
  Handle read_handle(read);
  Handle write_handle(write);
  HANDLE parent = child_reads ? write_handle.get() : read_handle.get();
  if (::SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0) == FALSE) {
    throw_card(CardError::backend_unavailable, "make ViKey parent pipe handle non-inheritable", DiagnosticStage::ipc_setup);
  }
  if (child_reads) {
    return {.parent_end = std::move(write_handle),
            .child_end = std::move(read_handle)};
  }
  return {.parent_end = std::move(read_handle),
          .child_end = std::move(write_handle)};
}

[[nodiscard]] Handle duplicate_inheritable_handle(const HANDLE source) {
  HANDLE duplicate = INVALID_HANDLE_VALUE;
  if (::DuplicateHandle(::GetCurrentProcess(), source,
                        ::GetCurrentProcess(), &duplicate, 0, TRUE,
                        DUPLICATE_SAME_ACCESS) == FALSE) {
    throw_card(CardError::backend_unavailable, "duplicate ViKey helper IPC handle", DiagnosticStage::ipc_setup);
  }
  return Handle(duplicate);
}

[[nodiscard]] Handle create_restricted_token() {
  HANDLE raw_token = INVALID_HANDLE_VALUE;
  if (::OpenProcessToken(::GetCurrentProcess(),
                         TOKEN_DUPLICATE | TOKEN_QUERY |
                             TOKEN_ASSIGN_PRIMARY | TOKEN_IMPERSONATE,
                         &raw_token) == FALSE) {
    throw_process_security_win32(7, "open current token for ViKey helper");
  }
  Handle source_token(raw_token);

  DWORD restricted_bytes = 0;
  static_cast<void>(::GetTokenInformation(source_token.get(),
                                          TokenRestrictedSids, nullptr, 0,
                                          &restricted_bytes));
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_process_security_win32(8, "size current token restrictions for ViKey helper");
  }
  if (restricted_bytes == 0) {
    throw_process_security(9, "invalid size: size current token restrictions for ViKey helper");
  }
  std::vector<std::byte> restricted_storage(restricted_bytes);
  if (::GetTokenInformation(source_token.get(), TokenRestrictedSids,
                            restricted_storage.data(), restricted_bytes,
                            &restricted_bytes) == FALSE) {
    throw_process_security_win32(10, "read current token restrictions for ViKey helper");
  }
  const auto* const existing_restrictions =
      reinterpret_cast<const TOKEN_GROUPS*>(restricted_storage.data());
  if (existing_restrictions->GroupCount != 0) {
    throw_process_security(11, "ViKey helper cannot safely refilter an already restricted "
               "parent token");
  }

  HANDLE raw_restricted = INVALID_HANDLE_VALUE;
  if (::CreateRestrictedToken(source_token.get(), DISABLE_MAX_PRIVILEGE, 0,
                              nullptr, 0, nullptr, 0, nullptr,
                              &raw_restricted) == FALSE) {
    throw_process_security_win32(12, "create restricted ViKey helper token");
  }
  return Handle(raw_restricted);
}

void filter_development_process_privileges(const HANDLE token) {
  LUID change_notify{};
  if (::LookupPrivilegeValueW(nullptr, L"SeChangeNotifyPrivilege",
                              &change_notify) == FALSE) {
    throw_process_security_win32(13, "resolve development ViKey helper traversal privilege");
  }

  DWORD privilege_bytes = 0;
  static_cast<void>(
      ::GetTokenInformation(token, TokenPrivileges, nullptr, 0,
                            &privilege_bytes));
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_process_security_win32(14, "size development ViKey helper token privileges");
  }
  if (privilege_bytes < sizeof(TOKEN_PRIVILEGES)) {
    throw_process_security(15, "invalid size: size development ViKey helper token privileges");
  }
  std::vector<std::byte> privilege_storage(privilege_bytes);
  if (::GetTokenInformation(token, TokenPrivileges, privilege_storage.data(),
                            privilege_bytes, &privilege_bytes) == FALSE) {
    throw_process_security_win32(16, "read development ViKey helper token privileges");
  }
  auto* const privileges =
      reinterpret_cast<TOKEN_PRIVILEGES*>(privilege_storage.data());
  for (DWORD index = 0; index < privileges->PrivilegeCount; ++index) {
    LUID_AND_ATTRIBUTES& privilege = privileges->Privileges[index];
    const bool is_change_notify =
        privilege.Luid.LowPart == change_notify.LowPart &&
        privilege.Luid.HighPart == change_notify.HighPart;
    privilege.Attributes =
        is_change_notify ? SE_PRIVILEGE_ENABLED : SE_PRIVILEGE_REMOVED;
  }

  ::SetLastError(ERROR_SUCCESS);
  if (::AdjustTokenPrivileges(token, FALSE, privileges, 0, nullptr,
                              nullptr) == FALSE ||
      ::GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
    throw_process_security_win32(17, "filter development ViKey helper token privileges");
  }
}

[[nodiscard]] Handle create_single_process_job() {
  Handle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job) {
    throw_process_security_win32(18, "create ViKey helper job object");
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  if (::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)) == FALSE) {
    throw_process_security_win32(19, "limit ViKey helper job object");
  }
  return job;
}

[[nodiscard]] std::uint32_t inspect_process_security(
    const HANDLE token, const HANDLE process, const HANDLE job,
    const PSID expected_app_container_sid) {
  std::uint32_t properties = 0;
  LUID change_notify{};
  if (::LookupPrivilegeValueW(nullptr, L"SeChangeNotifyPrivilege",
                              &change_notify) == FALSE) {
    throw_process_security_win32(20, "resolve ViKey helper traversal privilege");
  }

  DWORD returned = 0;
  static_cast<void>(
      ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &returned));
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_process_security_win32(21, "size ViKey helper token privileges");
  }
  if (returned < sizeof(TOKEN_PRIVILEGES)) {
    throw_process_security(22, "invalid size: size ViKey helper token privileges");
  }
  std::vector<std::byte> privilege_storage(returned);
  if (::GetTokenInformation(token, TokenPrivileges, privilege_storage.data(),
                            returned, &returned) == FALSE) {
    throw_process_security_win32(23, "inspect ViKey helper token privileges");
  }
  const auto* const privileges =
      reinterpret_cast<const TOKEN_PRIVILEGES*>(privilege_storage.data());
  for (DWORD index = 0; index < privileges->PrivilegeCount; ++index) {
    const LUID_AND_ATTRIBUTES& privilege = privileges->Privileges[index];
    const bool enabled = (privilege.Attributes & SE_PRIVILEGE_ENABLED) != 0;
    const bool is_change_notify =
        privilege.Luid.LowPart == change_notify.LowPart &&
        privilege.Luid.HighPart == change_notify.HighPart;
    if (expected_app_container_sid == nullptr && !is_change_notify) {
      throw_process_security(24, "development ViKey helper retained a removable privilege");
    }
    if (enabled && !is_change_notify) {
      throw_process_security(25, "ViKey helper token retained an enabled privilege");
    }
  }
  properties |=
      helper::property_bit(helper::SecurityProperty::filtered_token);

  TOKEN_ELEVATION elevation{};
  returned = 0;
  if (::GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &returned) == FALSE) {
    throw_process_security_win32(26, "inspect ViKey helper token elevation");
  }
  if (returned != sizeof(elevation)) {
    throw_process_security(27, "invalid helper elevation result size");
  }
  if (elevation.TokenIsElevated == 0) {
    properties |=
        helper::property_bit(helper::SecurityProperty::not_elevated);
  }

  BOOL is_app_container = FALSE;
  returned = 0;
  if (::GetTokenInformation(token, TokenIsAppContainer, &is_app_container,
                            sizeof(is_app_container), &returned) == FALSE) {
    throw_process_security_win32(28, "inspect ViKey helper AppContainer state");
  }
  if (returned != sizeof(is_app_container)) {
    throw_process_security(29, "invalid helper is_app_container result size");
  }
  if (expected_app_container_sid == nullptr) {
    if (is_app_container != FALSE) {
      throw_process_security(30, "development ViKey helper unexpectedly entered an "
                 "AppContainer");
    }
  } else {
    if (is_app_container == FALSE) {
      throw_process_security(31, "ViKey helper is not in an AppContainer");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::app_container);

    DWORD app_container_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenAppContainerSid, nullptr, 0, &app_container_bytes));
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_process_security_win32(32, "size ViKey helper AppContainer identity");
    }
    if (app_container_bytes < sizeof(TOKEN_APPCONTAINER_INFORMATION)) {
      throw_process_security(33, "invalid size: size ViKey helper AppContainer identity");
    }
    std::vector<std::byte> app_container_storage(app_container_bytes);
    if (::GetTokenInformation(token, TokenAppContainerSid,
                              app_container_storage.data(),
                              app_container_bytes,
                              &app_container_bytes) == FALSE) {
      throw_process_security_win32(34, "inspect ViKey helper AppContainer identity");
    }
    const auto* const app_container =
        reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(
            app_container_storage.data());
    if (app_container->TokenAppContainer == nullptr ||
        ::EqualSid(app_container->TokenAppContainer,
                   expected_app_container_sid) == FALSE) {
      throw_process_security(35, "ViKey helper AppContainer identity is invalid");
    }

    DWORD capability_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenCapabilities, nullptr, 0, &capability_bytes));
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_process_security_win32(36, "size ViKey helper capabilities");
    }
    if (capability_bytes < sizeof(DWORD)) {
      throw_process_security(37, "invalid size: size ViKey helper capabilities");
    }
    std::vector<std::byte> capability_storage(capability_bytes);
    if (::GetTokenInformation(token, TokenCapabilities,
                              capability_storage.data(), capability_bytes,
                              &capability_bytes) == FALSE) {
      throw_process_security_win32(38, "inspect ViKey helper capabilities");
    }
    const auto* const capabilities =
        reinterpret_cast<const TOKEN_GROUPS*>(capability_storage.data());
    if (capabilities->GroupCount != 0) {
      throw_process_security(39, "ViKey helper retained an AppContainer capability");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::no_capabilities);

    DWORD integrity_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenIntegrityLevel, nullptr, 0, &integrity_bytes));
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_process_security_win32(40, "size ViKey helper integrity level");
    }
    if (integrity_bytes < sizeof(TOKEN_MANDATORY_LABEL)) {
      throw_process_security(41, "invalid size: size ViKey helper integrity level");
    }
    std::vector<std::byte> integrity_storage(integrity_bytes);
    if (::GetTokenInformation(token, TokenIntegrityLevel,
                              integrity_storage.data(), integrity_bytes,
                              &integrity_bytes) == FALSE) {
      throw_process_security_win32(42, "inspect ViKey helper integrity level");
    }
    const auto* const integrity =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(
            integrity_storage.data());
    if (integrity->Label.Sid == nullptr ||
        ::IsValidSid(integrity->Label.Sid) == FALSE) {
      throw_process_security(43, "ViKey helper integrity SID is invalid");
    }
    const UCHAR subauthority_count =
        *::GetSidSubAuthorityCount(integrity->Label.Sid);
    if (subauthority_count == 0 ||
        *::GetSidSubAuthority(integrity->Label.Sid,
                              subauthority_count - 1) >
            SECURITY_MANDATORY_LOW_RID) {
      throw_process_security(44, "ViKey helper integrity level is too high");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::low_integrity);
  }

  BOOL in_job = FALSE;
  if (::IsProcessInJob(process, job, &in_job) == FALSE) {
    throw_process_security_win32(45, "inspect ViKey helper job membership");
  }
  if (in_job != FALSE) {
    properties |=
        helper::property_bit(helper::SecurityProperty::assigned_to_job);
  }

  PROCESS_MITIGATION_CHILD_PROCESS_POLICY child{};
  PROCESS_MITIGATION_DEP_POLICY dep{};
  PROCESS_MITIGATION_ASLR_POLICY aslr{};
  PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles{};
  PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensions{};
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY images{};
  if (::GetProcessMitigationPolicy(process, ProcessChildProcessPolicy, &child, sizeof(child)) == FALSE) {
    throw_process_security_win32(46, "inspect helper child mitigation");
  }
  if (::GetProcessMitigationPolicy(process, ProcessDEPPolicy, &dep, sizeof(dep)) == FALSE) {
    throw_process_security_win32(47, "inspect helper dep mitigation");
  }
  if (::GetProcessMitigationPolicy(process, ProcessASLRPolicy, &aslr, sizeof(aslr)) == FALSE) {
    throw_process_security_win32(48, "inspect helper aslr mitigation");
  }
  if (::GetProcessMitigationPolicy(process, ProcessStrictHandleCheckPolicy, &handles, sizeof(handles)) == FALSE) {
    throw_process_security_win32(49, "inspect helper handles mitigation");
  }
  if (::GetProcessMitigationPolicy(process, ProcessExtensionPointDisablePolicy, &extensions, sizeof(extensions)) == FALSE) {
    throw_process_security_win32(50, "inspect helper extensions mitigation");
  }
  if (::GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &images, sizeof(images)) == FALSE) {
    throw_process_security_win32(51, "inspect helper images mitigation");
  }
  if (child.NoChildProcessCreation != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::child_processes_disabled);
  }
  if (dep.Enable != 0) {
    properties |=
        helper::property_bit(helper::SecurityProperty::dep_enabled);
  }
  if (aslr.EnableBottomUpRandomization != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::bottom_up_aslr_enabled);
  }
  if (aslr.EnableHighEntropy != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::high_entropy_aslr_enabled);
  }
  if (handles.RaiseExceptionOnInvalidHandleReference != 0 &&
      handles.HandleExceptionsPermanentlyEnabled != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::strict_handle_checks);
  }
  if (extensions.DisableExtensionPoints != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::extension_points_disabled);
  }
  if (images.NoRemoteImages != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::remote_image_load_blocked);
  }
  if (images.NoLowMandatoryLabelImages != 0) {
    properties |= helper::property_bit(
        helper::SecurityProperty::low_label_image_load_blocked);
  }
  return properties;
}

[[nodiscard]] std::array<std::byte, helper::kNonceSize> random_nonce() {
  std::array<std::byte, helper::kNonceSize> nonce{};
  const NTSTATUS status = ::BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(nonce.data()),
      static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    throw_card(CardError::backend_unavailable,
               "generate ViKey helper IPC nonce");
  }
  return nonce;
}

struct EphemeralAppContainer final {
  EphemeralAppContainer(std::wstring profile_moniker, PSID profile_sid)
      : moniker(std::move(profile_moniker)), sid(profile_sid) {}

  std::wstring moniker;
  LocalSid sid;
  bool profile_exists{true};

  EphemeralAppContainer(const EphemeralAppContainer&) = delete;
  EphemeralAppContainer& operator=(const EphemeralAppContainer&) = delete;
  EphemeralAppContainer(EphemeralAppContainer&&) = delete;
  EphemeralAppContainer& operator=(EphemeralAppContainer&&) = delete;

  ~EphemeralAppContainer() noexcept {
    if (profile_exists) {
      static_cast<void>(::DeleteAppContainerProfile(moniker.c_str()));
    }
  }

  void remove_profile() {
    const HRESULT result = ::DeleteAppContainerProfile(moniker.c_str());
    if (FAILED(result)) {
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::cleanup, "remove helper profile", static_cast<std::uint32_t>(result), NativeCodeKind::hresult);
    }
    profile_exists = false;
  }
};

[[nodiscard]] std::unique_ptr<EphemeralAppContainer>
create_ephemeral_app_container() {
  constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
  const auto random = random_nonce();
  std::wstring moniker = L"usb-vault-vikey-helper-";
  moniker.reserve(moniker.size() + random.size() * 2);
  for (const std::byte value : random) {
    const unsigned int byte = std::to_integer<unsigned int>(value);
    moniker.push_back(kHexDigits[(byte >> 4U) & 0x0fU]);
    moniker.push_back(kHexDigits[byte & 0x0fU]);
  }
  PSID sid = nullptr;
  const HRESULT result = ::CreateAppContainerProfile(
      moniker.c_str(), L"usb-vault ViKey helper",
      L"Ephemeral zero-network ViKey helper boundary", nullptr, 0, &sid);
  if (FAILED(result) || sid == nullptr || ::IsValidSid(sid) == FALSE) {
    if (sid != nullptr) static_cast<void>(::FreeSid(sid));
    if (SUCCEEDED(result)) {
      static_cast<void>(::DeleteAppContainerProfile(moniker.c_str()));
    }
    throw_process_security(52, "create ephemeral ViKey helper AppContainer profile", FAILED(result) ? static_cast<std::uint32_t>(result) : 0, FAILED(result) ? NativeCodeKind::hresult : NativeCodeKind::none);
  }
  return std::make_unique<EphemeralAppContainer>(std::move(moniker), sid);
}

struct HelperLaunchContext final {
  std::vector<wchar_t> environment;
  std::wstring working_directory;
};

[[nodiscard]] std::vector<wchar_t> sanitized_environment(
    const std::wstring& local_app_data,
    const std::wstring* const windows_directory = nullptr) {
  const std::wstring temp = local_app_data + L"\\Temp";
  std::vector<wchar_t> environment;
  const auto append = [&environment](const std::wstring& entry) {
    environment.insert(environment.end(), entry.begin(), entry.end());
    environment.push_back(L'\0');
  };
  append(L"LOCALAPPDATA=" + local_app_data);
  append(L"TEMP=" + temp);
  append(L"TMP=" + temp);
  if (windows_directory != nullptr) {
    append(L"SystemRoot=" + *windows_directory);
    append(L"WINDIR=" + *windows_directory);
  }
  environment.push_back(L'\0');
  return environment;
}

[[nodiscard]] HelperLaunchContext app_container_launch_context(
    const PSID app_container_sid) {
  wchar_t* sid_string = nullptr;
  if (::ConvertSidToStringSidW(app_container_sid, &sid_string) == FALSE) {
    throw_process_security_win32(53, "format ViKey helper AppContainer identity");
  }
  if (sid_string == nullptr) {
    throw_process_security(54, "null formatted helper SID");
  }
  wchar_t* folder = nullptr;
  const HRESULT folder_result =
      ::GetAppContainerFolderPath(sid_string, &folder);
  static_cast<void>(::LocalFree(sid_string));
  if (FAILED(folder_result) || folder == nullptr) {
    if (folder != nullptr) ::CoTaskMemFree(folder);
    throw_card(CardError::backend_unavailable, "resolve ViKey helper AppContainer folder", DiagnosticStage::work_directory);
  }
  const std::wstring profile_folder(folder);
  ::CoTaskMemFree(folder);

  wchar_t* raw_virtual_local_app_data = nullptr;
  const HRESULT local_result = ::SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
      &raw_virtual_local_app_data);
  if (FAILED(local_result) || raw_virtual_local_app_data == nullptr) {
    if (raw_virtual_local_app_data != nullptr) {
      ::CoTaskMemFree(raw_virtual_local_app_data);
    }
    throw_card(CardError::backend_unavailable, "resolve AppContainer virtual LocalAppData path", DiagnosticStage::work_directory);
  }
  const std::wstring virtual_local_app_data(raw_virtual_local_app_data);
  ::CoTaskMemFree(raw_virtual_local_app_data);
  // AppContainer filesystem virtualization maps this conventional path into
  // profile_folder.  Passing profile_folder itself would be redirected a
  // second time when the child opens or maps an image by absolute path.
  return {.environment = sanitized_environment(virtual_local_app_data),
          .working_directory = profile_folder};
}

struct EphemeralDevelopmentDirectory final {
  explicit EphemeralDevelopmentDirectory(std::filesystem::path directory)
      : root(std::move(directory)), temp(root / L"Temp") {}

  std::filesystem::path root;
  std::filesystem::path temp;
  bool root_exists{true};
  bool temp_exists{true};

  EphemeralDevelopmentDirectory(const EphemeralDevelopmentDirectory&) =
      delete;
  EphemeralDevelopmentDirectory& operator=(
      const EphemeralDevelopmentDirectory&) = delete;

  ~EphemeralDevelopmentDirectory() noexcept {
    if (temp_exists) static_cast<void>(::RemoveDirectoryW(temp.c_str()));
    if (root_exists) static_cast<void>(::RemoveDirectoryW(root.c_str()));
  }

  void remove_directories() {
    DWORD temp_error = ERROR_SUCCESS;
    if (temp_exists && ::RemoveDirectoryW(temp.c_str()) == FALSE) {
      temp_error = ::GetLastError();
      if (temp_error == ERROR_FILE_NOT_FOUND ||
          temp_error == ERROR_PATH_NOT_FOUND) {
        temp_exists = false;
        temp_error = ERROR_SUCCESS;
      }
    } else {
      temp_exists = false;
    }

    DWORD root_error = ERROR_SUCCESS;
    if (root_exists && ::RemoveDirectoryW(root.c_str()) == FALSE) {
      root_error = ::GetLastError();
      if (root_error == ERROR_FILE_NOT_FOUND ||
          root_error == ERROR_PATH_NOT_FOUND) {
        root_exists = false;
        root_error = ERROR_SUCCESS;
      }
    } else {
      root_exists = false;
    }
    const DWORD error = root_error != ERROR_SUCCESS ? root_error : temp_error;
    if (error != ERROR_SUCCESS) {
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::cleanup, "remove helper directory", error, NativeCodeKind::win32);
    }
  }
};

[[nodiscard]] std::unique_ptr<EphemeralDevelopmentDirectory>
create_ephemeral_development_directory() {
  wchar_t* raw_local_app_data = nullptr;
  const HRESULT local_result = ::SHGetKnownFolderPath(
      FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
      &raw_local_app_data);
  if (FAILED(local_result) || raw_local_app_data == nullptr) {
    if (raw_local_app_data != nullptr) ::CoTaskMemFree(raw_local_app_data);
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::work_directory, "resolve helper directory", static_cast<std::uint32_t>(local_result), NativeCodeKind::hresult);
  }
  const std::filesystem::path local_app_data(raw_local_app_data);
  ::CoTaskMemFree(raw_local_app_data);

  constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
  for (unsigned int attempt = 0; attempt < 8; ++attempt) {
    const auto random = random_nonce();
    std::wstring name(kDevelopmentDirectoryPrefix);
    name.reserve(name.size() + random.size() * 2);
    for (const std::byte value : random) {
      const unsigned int byte = std::to_integer<unsigned int>(value);
      name.push_back(kHexDigits[(byte >> 4U) & 0x0fU]);
      name.push_back(kHexDigits[byte & 0x0fU]);
    }
    const std::filesystem::path root = local_app_data / name;
    if (::CreateDirectoryW(root.c_str(), nullptr) == FALSE) {
      if (::GetLastError() == ERROR_ALREADY_EXISTS) continue;
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::work_directory, "create helper directory", ::GetLastError(), NativeCodeKind::win32);
    }
    const std::filesystem::path temp = root / L"Temp";
    if (::CreateDirectoryW(temp.c_str(), nullptr) == FALSE) {
      const DWORD error = ::GetLastError();
      if (::RemoveDirectoryW(root.c_str()) == FALSE) {
        throw_card(CardError::backend_unavailable, "remove incomplete development ViKey helper directory", DiagnosticStage::cleanup);
      }
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::work_directory, "create helper temp", error, NativeCodeKind::win32);
    }
    return std::make_unique<EphemeralDevelopmentDirectory>(root);
  }
  throw_card(CardError::backend_unavailable, "allocate unique development ViKey helper directory", DiagnosticStage::work_directory);
}

[[nodiscard]] HelperLaunchContext development_launch_context(
    const EphemeralDevelopmentDirectory& directory) {
  const std::wstring root = directory.root.native();
  std::array<wchar_t, MAX_PATH> windows_directory{};
  const UINT written = ::GetWindowsDirectoryW(
      windows_directory.data(), static_cast<UINT>(windows_directory.size()));
  if (written == 0 || written >= windows_directory.size()) {
    throw_card(CardError::backend_unavailable,
               "resolve development ViKey helper Windows directory");
  }
  const std::wstring windows(windows_directory.data(), written);
  return {.environment = sanitized_environment(root, &windows),
          .working_directory = root};
}

[[nodiscard]] VerifiedLocalArtifact stage_vendor_dll(
    const VerifiedLocalArtifact& source,
    const Sha256Digest& expected_digest,
    const std::filesystem::path& profile_folder) {
  const std::filesystem::path staged = profile_folder / kVendorDllName;
  if (::CopyFileW(source.path().c_str(), staged.c_str(), TRUE) == FALSE) {
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::sdk_stage, "stage SDK", ::GetLastError(), NativeCodeKind::win32);
  }

  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"S:(ML;;NW;;;ME)", SDDL_REVISION_1, &raw_descriptor,
          nullptr) == FALSE ||
      raw_descriptor == nullptr) {
    if (raw_descriptor != nullptr) ::LocalFree(raw_descriptor);
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::sdk_stage, "create SDK security label");
  }
  BOOL sacl_present = FALSE;
  BOOL sacl_defaulted = FALSE;
  PACL mandatory_label = nullptr;
  const BOOL read_label = ::GetSecurityDescriptorSacl(
      raw_descriptor, &sacl_present, &mandatory_label, &sacl_defaulted);
  const DWORD label_result =
      read_label != FALSE && sacl_present != FALSE &&
              mandatory_label != nullptr
          ? ::SetNamedSecurityInfoW(
                const_cast<wchar_t*>(staged.c_str()), SE_FILE_OBJECT,
                LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                mandatory_label)
          : ERROR_INVALID_SECURITY_DESCR;
  ::LocalFree(raw_descriptor);
  if (label_result != ERROR_SUCCESS) {
    throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::sdk_stage, "label staged SDK", label_result, NativeCodeKind::win32);
  }
  return verify_local_artifact(staged, expected_digest, kMaximumVendorBytes);
}

void remove_staged_vendor(
    std::optional<VerifiedLocalArtifact>& staged_vendor,
    const std::filesystem::path& staged_path) {
  staged_vendor.reset();
  if (staged_path.empty()) return;
  if (::DeleteFileW(staged_path.c_str()) == FALSE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND) {
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::cleanup, "remove staged SDK", error, NativeCodeKind::win32);
    }
  }
}

void cleanup_app_container(
    EphemeralAppContainer& app_container,
    std::optional<VerifiedLocalArtifact>& staged_vendor,
    const std::filesystem::path& staged_path) {
  std::exception_ptr staged_failure;
  try {
    remove_staged_vendor(staged_vendor, staged_path);
  } catch (...) {
    staged_failure = std::current_exception();
  }
  cleanup_preserving_failure(staged_failure,
      [&] { app_container.remove_profile(); });
}

void cleanup_development_directory(
    EphemeralDevelopmentDirectory& directory,
    std::optional<VerifiedLocalArtifact>& staged_vendor,
    const std::filesystem::path& staged_path) {
  std::exception_ptr staged_failure;
  try {
    remove_staged_vendor(staged_vendor, staged_path);
  } catch (...) {
    staged_failure = std::current_exception();
  }
  // Directory cleanup is observable because a leftover development staging
  // area could otherwise be mistaken for a supported long-lived SDK install.
  cleanup_preserving_failure(staged_failure,
      [&] { directory.remove_directories(); });
}

[[nodiscard]] helper::Response read_response_until(
    const HANDLE pipe, const HANDLE process,
    const std::array<std::byte, helper::kNonceSize>& nonce,
    const helper::Operation operation,
    const std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    DWORD available = 0;
    if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ==
        FALSE) {
      throw_card(CardError::communication_failed, "inspect ViKey helper IPC response", DiagnosticStage::ipc_read);
    }
    if (available >= sizeof(helper::Response)) {
      return helper::read_response(pipe, nonce, operation);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw_card(CardError::operation_timed_out, "ViKey helper IPC response timed out", DiagnosticStage::response_timeout);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const DWORD wait_milliseconds = static_cast<DWORD>(
        std::clamp<std::int64_t>(remaining.count(), 1, 10));
    const DWORD wait = ::WaitForSingleObject(process, wait_milliseconds);
    if (wait == WAIT_FAILED) {
      throw_card(CardError::communication_failed, "wait for ViKey helper response", DiagnosticStage::ipc_read);
    }
    if (wait == WAIT_OBJECT_0) {
      if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) !=
              FALSE &&
          available >= sizeof(helper::Response)) {
        return helper::read_response(pipe, nonce, operation);
      }
      DWORD exit_code = 0;
      if (::GetExitCodeProcess(process, &exit_code) != FALSE) {
        throw DiagnosticError(make_error_code(CardError::communication_failed), DiagnosticStage::process_exit, "helper exited", exit_code, NativeCodeKind::process_exit);
      }
      throw_card(CardError::communication_failed, "ViKey helper exited without a complete response", DiagnosticStage::process_exit);
    }
  }
}

[[nodiscard]] security::SecureBuffer read_secret_payload_until(
    const HANDLE pipe, const HANDLE process,
    const helper::Response& response,
    const helper::Operation operation, const DWORD expected_bytes,
    const std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    DWORD available = 0;
    if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ==
        FALSE) {
      throw_card(CardError::communication_failed, "inspect ViKey helper secret response", DiagnosticStage::ipc_read);
    }
    if (available >= expected_bytes) {
      return operation == helper::Operation::card_record_read
                 ? helper::read_card_record_response_payload(pipe, response)
                 : helper::read_development_blob_response_payload(pipe,
                                                                    response);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw_card(CardError::operation_timed_out, "ViKey helper secret response timed out", DiagnosticStage::data_timeout);
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const DWORD wait_milliseconds = static_cast<DWORD>(
        std::clamp<std::int64_t>(remaining.count(), 1, 10));
    const DWORD wait = ::WaitForSingleObject(process, wait_milliseconds);
    if (wait == WAIT_FAILED) {
      throw_card(CardError::communication_failed, "wait for ViKey helper secret response", DiagnosticStage::ipc_read);
    }
    if (wait == WAIT_OBJECT_0) {
      if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) !=
              FALSE &&
          available >= expected_bytes) {
        return operation == helper::Operation::card_record_read
                   ? helper::read_card_record_response_payload(pipe,
                                                                response)
                   : helper::read_development_blob_response_payload(
                         pipe, response);
      }
      throw_card(CardError::communication_failed, "ViKey helper exited without a complete secret response", DiagnosticStage::process_exit);
    }
  }
}

void require_clean_exit(const HANDLE process, const HANDLE response_pipe,
                        const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    throw_card(CardError::communication_failed, "ViKey helper did not exit before its deadline", DiagnosticStage::process_exit);
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - now);
  const auto bounded = std::clamp<std::int64_t>(
      remaining.count(), 1, std::numeric_limits<DWORD>::max());
  if (::WaitForSingleObject(process, static_cast<DWORD>(bounded)) !=
      WAIT_OBJECT_0) {
    throw_card(CardError::communication_failed, "ViKey helper did not exit cleanly", DiagnosticStage::process_exit);
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process, &exit_code) == FALSE) {
    throw_card(CardError::communication_failed, "read ViKey helper exit code", DiagnosticStage::process_exit);
  }
  if (exit_code != 0) {
    throw DiagnosticError(make_error_code(CardError::communication_failed), DiagnosticStage::process_exit, "helper failure exit", exit_code, NativeCodeKind::process_exit);
  }
  DWORD trailing = 0;
  if (::PeekNamedPipe(response_pipe, nullptr, 0, nullptr, &trailing,
                      nullptr) != FALSE) {
    if (trailing == 0) return;
    throw_card(CardError::communication_failed,
               "ViKey helper returned trailing IPC data");
  }
  if (::GetLastError() != ERROR_BROKEN_PIPE) {
    throw_card(CardError::communication_failed,
               "inspect ViKey helper trailing IPC data");
  }
}

}  // namespace

namespace {

[[nodiscard]] VikeyHelperProbe probe_vikey_helper_operation(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const helper::Operation operation,
    const std::filesystem::path* const vendor_path,
    const Sha256Digest* const vendor_digest,
    const DevelopmentSessionInputs* const session_inputs,
    std::uint32_t* const success_detail,
    const HelperBoundary boundary,
    const std::chrono::milliseconds timeout,
    security::SecureBuffer* const blob_response = nullptr,
    std::uint32_t* const success_result = nullptr,
    std::uint32_t* const success_logical_state = nullptr) {
  CardOperationLease operation_lease;
  if (timeout <= std::chrono::milliseconds::zero()) {
    throw_card(CardError::invalid_configuration,
               "ViKey helper timeout must be positive");
  }
  const bool session_required =
      operation == helper::Operation::development_session_probe ||
      operation == helper::Operation::card_identity_probe ||
      operation == helper::Operation::card_media_probe ||
      operation == helper::Operation::card_media_bootstrap ||
      operation == helper::Operation::development_media_reset ||
      operation ==
          helper::Operation::development_blob_round_trip_probe ||
      operation == helper::Operation::development_blob_read ||
      operation == helper::Operation::development_blob_write ||
      operation == helper::Operation::card_record_read ||
      operation == helper::Operation::card_record_write;
  const bool blob_input_required =
      operation == helper::Operation::development_blob_round_trip_probe ||
      operation == helper::Operation::development_blob_write ||
      operation == helper::Operation::card_record_write;
  const bool blob_output_required =
      operation == helper::Operation::development_blob_read ||
      operation == helper::Operation::card_record_read;
  const bool vendor_required =
      operation == helper::Operation::vendor_runtime_probe ||
      session_required;
  const bool expected_identity_required =
      operation == helper::Operation::card_record_read ||
      operation == helper::Operation::card_record_write ||
      operation == helper::Operation::card_media_bootstrap ||
      operation == helper::Operation::development_media_reset;
  const bool expected_media_state_required =
      operation == helper::Operation::card_media_bootstrap ||
      operation == helper::Operation::development_media_reset;
  const bool success_detail_required =
      operation == helper::Operation::development_session_probe ||
      operation == helper::Operation::card_identity_probe ||
      operation == helper::Operation::card_media_probe;
  const bool success_result_required =
      operation == helper::Operation::card_media_probe;
  const bool success_logical_state_required =
      operation == helper::Operation::card_media_probe;
  if (vendor_required !=
      (vendor_path != nullptr && vendor_digest != nullptr)) {
    throw_card(CardError::invalid_configuration,
               "ViKey vendor probe configuration is incomplete");
  }
  if (session_required != (session_inputs != nullptr) ||
      success_detail_required != (success_detail != nullptr) ||
      success_result_required != (success_result != nullptr) ||
      success_logical_state_required !=
          (success_logical_state != nullptr) ||
      blob_output_required != (blob_response != nullptr) ||
      (session_required &&
       boundary != HelperBoundary::network_enabled_development)) {
    throw_card(CardError::invalid_configuration,
               "ViKey development session configuration is incomplete");
  }
  if (session_inputs != nullptr &&
      (session_inputs->user_sn.size() != kVikeyUserSnBytes ||
       session_inputs->api_key.size() != kVikeyApiKeyBytes ||
       session_inputs->user_pin.size() != kVikeyUserPinBytes ||
       expected_identity_required !=
           (session_inputs->expected_hardware_id != 0) ||
       expected_media_state_required !=
           (operation == helper::Operation::development_media_reset
                ? helper::valid_development_reset_media_state(
                      session_inputs->expected_media_state)
                : helper::valid_bootstrap_media_state(
                      session_inputs->expected_media_state)) ||
       blob_input_required != (session_inputs->blob != nullptr) ||
       (session_inputs->blob != nullptr &&
        session_inputs->blob->size() !=
            (operation == helper::Operation::card_record_write
                 ? kCardRecordV1Size
                 : kVikeyBlobBytes)))) {
    throw_card(CardError::invalid_configuration, "ViKey development session credential size is invalid", DiagnosticStage::input_shape);
  }
  VerifiedLocalArtifact executable_artifact = verify_local_artifact(
      helper_path, expected_digest, kMaximumHelperBytes);
  std::optional<VerifiedLocalArtifact> vendor_source;
  if (vendor_required) {
    vendor_source.emplace(verify_local_artifact(
        *vendor_path, *vendor_digest, kMaximumVendorBytes));
  }
  const std::filesystem::path& executable = executable_artifact.path();
  Pipe request_pipe = create_pipe(true);
  Pipe response_pipe = create_pipe(false);
  Handle error_pipe =
      duplicate_inheritable_handle(response_pipe.child_end.get());
  Handle token;
  if (boundary == HelperBoundary::zero_network_app_container) {
    token = create_restricted_token();
  }
  Handle job = create_single_process_job();
  std::array<HANDLE, 3> inherited_handles{
      request_pipe.child_end.get(), response_pipe.child_end.get(),
      error_pipe.get()};
  DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
  DWORD64 mitigations = kHelperMitigations;
  AttributeList attributes(4);
  attributes.add(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles.data(),
                 sizeof(inherited_handles), 3);
  attributes.add(PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
                 sizeof(mitigations), 4);
  attributes.add(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &child_policy,
                 sizeof(child_policy), 5);

  std::unique_ptr<EphemeralAppContainer> app_container;
  std::unique_ptr<EphemeralDevelopmentDirectory> development_directory;
  SECURITY_CAPABILITIES security_capabilities{};
  HelperLaunchContext launch_context;
  std::filesystem::path staged_vendor_path;
  std::optional<VerifiedLocalArtifact> staged_vendor;
  const auto cleanup_boundary = [&]() {
    if (app_container) {
      cleanup_app_container(*app_container, staged_vendor,
                            staged_vendor_path);
    } else if (development_directory) {
      cleanup_development_directory(*development_directory, staged_vendor,
                                    staged_vendor_path);
    }
  };
  try {
    if (boundary == HelperBoundary::zero_network_app_container) {
      app_container = create_ephemeral_app_container();
      security_capabilities.AppContainerSid = app_container->sid.get();
      launch_context =
          app_container_launch_context(app_container->sid.get());
      attributes.add(PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                     &security_capabilities, sizeof(security_capabilities), 6);
    } else {
      development_directory = create_ephemeral_development_directory();
      launch_context =
          development_launch_context(*development_directory);
    }
    if (vendor_source && vendor_digest != nullptr) {
      staged_vendor_path =
          std::filesystem::path(launch_context.working_directory) /
          kVendorDllName;
      staged_vendor.emplace(stage_vendor_dll(
          *vendor_source, *vendor_digest,
          launch_context.working_directory));
    }
  } catch (...) {
    // This is still before process creation, so ephemeral resource removal is
    // synchronous and any cleanup failure remains observable.
    cleanup_preserving_failure(std::current_exception(), cleanup_boundary);
    throw;  // Unreachable: the original failure always propagates.
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = request_pipe.child_end.get();
  startup.StartupInfo.hStdOutput = response_pipe.child_end.get();
  startup.StartupInfo.hStdError = error_pipe.get();
  startup.lpAttributeList = attributes.get();
  PROCESS_INFORMATION process_info{};
  std::wstring command = std::format(L"\"{}\"", executable.native());
  constexpr DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT |
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                   CREATE_UNICODE_ENVIRONMENT;
  const BOOL launched =
      boundary == HelperBoundary::zero_network_app_container
          ? ::CreateProcessAsUserW(
                token.get(), executable.c_str(), command.data(), nullptr,
                nullptr, TRUE, creation_flags,
                launch_context.environment.data(),
                launch_context.working_directory.c_str(),
                &startup.StartupInfo, &process_info)
          : ::CreateProcessW(
                executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                creation_flags, launch_context.environment.data(),
                launch_context.working_directory.c_str(),
                &startup.StartupInfo, &process_info);
  if (launched == FALSE) {
    const DWORD error = ::GetLastError();
    // Do not leave an ephemeral profile or development staging directory
    // behind when the verified artifact cannot be started. Preserve both errors.
    try {
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::process_start, "start helper", error, NativeCodeKind::win32);
    } catch (...) {
      cleanup_preserving_failure(std::current_exception(), cleanup_boundary);
      throw;
    }
  }
  Handle process(process_info.hProcess);
  Handle thread(process_info.hThread);
  request_pipe.child_end.reset();
  response_pipe.child_end.reset();
  error_pipe.reset();

  const std::uint32_t required_security_properties =
      boundary == HelperBoundary::zero_network_app_container
          ? helper::kRequiredSecurityProperties
          : helper::kDevelopmentCompatibilitySecurityProperties;
  const std::uint32_t parent_required_security_properties =
      required_security_properties &
      ~helper::property_bit(
          helper::SecurityProperty::sanitized_environment);
  std::uint32_t parent_security_properties = 0;
  VikeyHelperProbe result{};
  try {
    HANDLE raw_process_token = INVALID_HANDLE_VALUE;
    const DWORD token_access =
        boundary == HelperBoundary::network_enabled_development
            ? TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES
            : TOKEN_QUERY;
    if (::OpenProcessToken(process.get(), token_access,
                           &raw_process_token) == FALSE) {
      throw_process_security_win32(55, "open ViKey helper process token");
    }
    Handle process_token(raw_process_token);

    if (boundary == HelperBoundary::network_enabled_development) {
      // The primary thread is still suspended. Remove every privilege except
      // directory traversal before any helper or vendor instruction executes.
      filter_development_process_privileges(process_token.get());
    }

    if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
      throw_process_security_win32(56, "assign ViKey helper to job object");
    }
    parent_security_properties =
        inspect_process_security(process_token.get(), process.get(), job.get(),
                                 app_container ? app_container->sid.get()
                                               : nullptr);
    require_security_properties(parent_security_properties, parent_required_security_properties, {
        {helper::SecurityProperty::filtered_token, 57},
        {helper::SecurityProperty::not_elevated, 58},
        {helper::SecurityProperty::app_container, 59},
        {helper::SecurityProperty::no_capabilities, 60},
        {helper::SecurityProperty::low_integrity, 61},
        {helper::SecurityProperty::assigned_to_job, 62},
        {helper::SecurityProperty::child_processes_disabled, 63},
        {helper::SecurityProperty::dep_enabled, 64},
        {helper::SecurityProperty::bottom_up_aslr_enabled, 65},
        {helper::SecurityProperty::high_entropy_aslr_enabled, 66},
        {helper::SecurityProperty::strict_handle_checks, 67},
        {helper::SecurityProperty::extension_points_disabled, 68},
        {helper::SecurityProperty::remote_image_load_blocked, 69},
        {helper::SecurityProperty::low_label_image_load_blocked, 70},
        {helper::SecurityProperty::sanitized_environment, 71},
    });
    if (parent_security_properties !=
        parent_required_security_properties) {
      throw_process_security(72, "ViKey helper security boundary is incomplete");
    }
    if (::ResumeThread(thread.get()) == std::numeric_limits<DWORD>::max()) {
      throw DiagnosticError(make_error_code(CardError::backend_unavailable), DiagnosticStage::process_start, "resume helper", ::GetLastError(), NativeCodeKind::win32);
    }
    thread.reset();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    helper::Request request{};
    request.operation = operation;
    request.nonce = random_nonce();
    if (session_inputs != nullptr && vendor_digest != nullptr) {
      if (operation ==
          helper::Operation::development_blob_round_trip_probe) {
        request.payload_bytes =
            helper::kDevelopmentBlobRoundTripPayloadBytes;
        helper::write_development_blob_round_trip_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin, *session_inputs->blob);
      } else if (operation == helper::Operation::development_blob_read) {
        request.payload_bytes = helper::kDevelopmentBlobReadPayloadBytes;
        helper::write_development_blob_read_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else if (operation == helper::Operation::development_blob_write) {
        request.payload_bytes = helper::kDevelopmentBlobWritePayloadBytes;
        helper::write_development_blob_write_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin, *session_inputs->blob);
      } else if (operation == helper::Operation::card_record_read) {
        request.payload_bytes = helper::kCardRecordReadPayloadBytes;
        helper::write_card_record_read_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->expected_hardware_id,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else if (operation == helper::Operation::card_record_write) {
        request.payload_bytes = helper::kCardRecordWritePayloadBytes;
        helper::write_card_record_write_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->expected_hardware_id,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin, *session_inputs->blob);
      } else if (operation == helper::Operation::card_identity_probe) {
        request.payload_bytes = helper::kCardIdentityProbePayloadBytes;
        helper::write_card_identity_probe_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else if (operation == helper::Operation::card_media_probe) {
        request.payload_bytes = helper::kCardMediaProbePayloadBytes;
        helper::write_card_media_probe_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else if (operation == helper::Operation::card_media_bootstrap) {
        request.payload_bytes = helper::kCardMediaBootstrapPayloadBytes;
        helper::write_card_media_bootstrap_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->expected_hardware_id,
            session_inputs->expected_media_state,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else if (operation == helper::Operation::development_media_reset) {
        request.payload_bytes = helper::kDevelopmentMediaResetPayloadBytes;
        helper::write_development_media_reset_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->expected_hardware_id,
            session_inputs->expected_media_state,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      } else {
        request.payload_bytes = helper::kDevelopmentSessionPayloadBytes;
        helper::write_development_session_request(
            request_pipe.parent_end.get(), request, *vendor_digest,
            session_inputs->user_sn, session_inputs->api_key,
            session_inputs->user_pin);
      }
    } else {
      helper::write_request(request_pipe.parent_end.get(), request);
    }
    request_pipe.parent_end.reset();

    const helper::Response response = read_response_until(
        response_pipe.parent_end.get(), process.get(), request.nonce,
        operation, deadline);
    if (response.status != 0) {
      throw DiagnosticError(make_error_code(static_cast<CardError>(response.status)),
          static_cast<DiagnosticStage>(response.diagnostic), "helper operation failed",
          response.diagnostic ? response.detail : 0,
          static_cast<NativeCodeKind>(response.native_kind));
    }
    const std::uint32_t security_properties =
        parent_security_properties | response.security_properties;
    require_security_properties(security_properties, required_security_properties, {
        {helper::SecurityProperty::filtered_token, 73},
        {helper::SecurityProperty::not_elevated, 74},
        {helper::SecurityProperty::app_container, 75},
        {helper::SecurityProperty::no_capabilities, 76},
        {helper::SecurityProperty::low_integrity, 77},
        {helper::SecurityProperty::assigned_to_job, 78},
        {helper::SecurityProperty::child_processes_disabled, 79},
        {helper::SecurityProperty::dep_enabled, 80},
        {helper::SecurityProperty::bottom_up_aslr_enabled, 81},
        {helper::SecurityProperty::high_entropy_aslr_enabled, 82},
        {helper::SecurityProperty::strict_handle_checks, 83},
        {helper::SecurityProperty::extension_points_disabled, 84},
        {helper::SecurityProperty::remote_image_load_blocked, 85},
        {helper::SecurityProperty::low_label_image_load_blocked, 86},
        {helper::SecurityProperty::sanitized_environment, 87},
    });
    if (security_properties != required_security_properties) {
      throw_process_security(88, "ViKey helper security boundary is incomplete");
    }
    if (blob_response != nullptr) {
      const DWORD expected_bytes =
          operation == helper::Operation::card_record_read
              ? static_cast<DWORD>(kCardRecordV1Size)
              : static_cast<DWORD>(kVikeyBlobBytes);
      *blob_response = read_secret_payload_until(
          response_pipe.parent_end.get(), process.get(), response, operation,
          expected_bytes, deadline);
    }
    require_clean_exit(process.get(), response_pipe.parent_end.get(),
                       deadline);
    if (success_detail != nullptr) {
      *success_detail = response.detail;
    }
    if (success_result != nullptr) {
      *success_result = response.result;
    }
    if (success_logical_state != nullptr) {
      *success_logical_state = response.logical_state;
    }
    result.security_properties = security_properties;
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    static_cast<void>(::TerminateProcess(process.get(), 1));
    static_cast<void>(::WaitForSingleObject(process.get(), 1000));
    // Ephemeral launch resources are removed on every observed failure path.
    // Keep the original reason even when removing those resources also fails.
    cleanup_preserving_failure(failure, cleanup_boundary);
    std::rethrow_exception(failure);
  }
  cleanup_boundary();
  return result;
}

}  // namespace

VikeyHelperProbe probe_vikey_helper(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::probe, nullptr,
      nullptr, nullptr, nullptr,
      HelperBoundary::zero_network_app_container, timeout);
}

VikeyHelperProbe probe_vikey_vendor_runtime(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  return probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::vendor_runtime_probe, &vendor_path,
      &expected_vendor_digest, nullptr, nullptr,
      HelperBoundary::zero_network_app_container, timeout);
}

VikeyHelperProbe probe_vikey_hid_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::hid_access_probe,
      nullptr, nullptr, nullptr, nullptr,
      HelperBoundary::zero_network_app_container, timeout);
}

VikeyHelperProbe probe_vikey_development_helper_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::probe, nullptr,
      nullptr, nullptr, nullptr,
      HelperBoundary::network_enabled_development, timeout);
}

VikeyHelperProbe
probe_vikey_development_vendor_runtime_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  return probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::vendor_runtime_probe, &vendor_path,
      &expected_vendor_digest, nullptr, nullptr,
      HelperBoundary::network_enabled_development, timeout);
}

VikeyHelperProbe
probe_vikey_development_hid_access_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::hid_access_probe,
      nullptr, nullptr, nullptr, nullptr,
      HelperBoundary::network_enabled_development, timeout);
}

VikeyDevelopmentSessionProbe
probe_vikey_development_session_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr};
  std::uint32_t packed_observation = 0;
  const VikeyHelperProbe probe = probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_session_probe, &vendor_path,
      &expected_vendor_digest, &inputs, &packed_observation,
      HelperBoundary::network_enabled_development, timeout);
  const helper::DevelopmentSessionObservation observation =
      helper::unpack_development_session_observation(packed_observation);
  return {
      .security_properties = probe.security_properties,
      .attempts_before = observation.attempts_before,
      .maximum_before = observation.maximum_before,
      .attempts_after = observation.attempts_after,
      .maximum_after = observation.maximum_after,
  };
}

VikeyCardIdentityProbe
probe_vikey_development_card_identity_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr,
      .expected_hardware_id = 0};
  std::uint32_t hardware_id = 0;
  const VikeyHelperProbe probe = probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::card_identity_probe, &vendor_path,
      &expected_vendor_digest, &inputs, &hardware_id,
      HelperBoundary::network_enabled_development, timeout);
  if (hardware_id == 0) {
    throw_card(CardError::device_failure,
               "ViKey helper returned a zero hardware identity");
  }
  return {.security_properties = probe.security_properties,
          .hardware_id = hardware_id};
}

VikeyCardMediaProbe
probe_vikey_development_card_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr,
      .expected_hardware_id = 0};
  std::uint32_t hardware_id = 0;
  std::uint32_t wire_state = 0;
  std::uint32_t wire_logical_state = 0;
  const VikeyHelperProbe probe = probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::card_media_probe, &vendor_path,
      &expected_vendor_digest, &inputs, &hardware_id,
      HelperBoundary::network_enabled_development, timeout, nullptr,
      &wire_state, &wire_logical_state);

  VikeyCardMediaState media_state{};
  switch (static_cast<helper::CardMediaState>(wire_state)) {
    case helper::CardMediaState::blank:
      media_state = VikeyCardMediaState::blank;
      break;
    case helper::CardMediaState::legacy_p1_or_foreign_prefix:
      media_state = VikeyCardMediaState::legacy_p1_or_foreign_prefix;
      break;
    case helper::CardMediaState::foreign_data:
      media_state = VikeyCardMediaState::foreign_data;
      break;
    case helper::CardMediaState::provisioning_interrupted:
      media_state = VikeyCardMediaState::provisioning_interrupted;
      break;
    case helper::CardMediaState::conflicting_evidence:
      media_state = VikeyCardMediaState::conflicting_evidence;
      break;
    case helper::CardMediaState::product_evidence:
      media_state = VikeyCardMediaState::product_evidence;
      break;
    default:
      throw_card(CardError::communication_failed,
                 "ViKey helper returned an invalid card media state");
  }

  std::optional<CardRecordState> logical_state;
  if (media_state == VikeyCardMediaState::product_evidence) {
    switch (static_cast<helper::CardLogicalState>(wire_logical_state)) {
      case helper::CardLogicalState::provisioned_uninitialized:
        logical_state = CardRecordState::provisioned_uninitialized;
        break;
      case helper::CardLogicalState::initialized_v1:
        logical_state = CardRecordState::initialized_v1;
        break;
      case helper::CardLogicalState::unsupported_version:
        logical_state = CardRecordState::unsupported_version;
        break;
      case helper::CardLogicalState::malformed:
        logical_state = CardRecordState::malformed;
        break;
      default:
        throw_card(CardError::communication_failed,
                   "ViKey helper returned an invalid logical card state");
    }
  } else if (wire_logical_state != 0) {
    throw_card(CardError::communication_failed,
               "ViKey helper returned a logical state without product evidence");
  }
  return {.security_properties = probe.security_properties,
          .hardware_id = hardware_id,
          .media_state = media_state,
          .logical_state = logical_state};
}

void bootstrap_vikey_development_card_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::uint32_t expected_hardware_id,
    const VikeyCardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  helper::CardMediaState wire_state{};
  switch (expected_media_state) {
    case VikeyCardMediaState::blank:
      wire_state = helper::CardMediaState::blank;
      break;
    case VikeyCardMediaState::provisioning_interrupted:
      wire_state = helper::CardMediaState::provisioning_interrupted;
      break;
    default:
      throw_card(CardError::invalid_configuration,
                 "ViKey bootstrap media state is not eligible");
  }

  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr,
      .expected_hardware_id = expected_hardware_id,
      .expected_media_state = wire_state};
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::card_media_bootstrap, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout));
}

void reset_vikey_development_legacy_media_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::uint32_t expected_hardware_id,
    const VikeyCardMediaState expected_media_state,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  if (expected_media_state !=
      VikeyCardMediaState::legacy_p1_or_foreign_prefix) {
    throw_card(CardError::invalid_configuration,
               "ViKey development reset media state is not eligible");
  }

  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr,
      .expected_hardware_id = expected_hardware_id,
      .expected_media_state =
          helper::CardMediaState::legacy_p1_or_foreign_prefix};
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_media_reset, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout));
}

VikeyHelperProbe
probe_vikey_development_blob_round_trip_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& test_blob,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = &test_blob};
  return probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_blob_round_trip_probe, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout);
}

security::SecureBuffer read_vikey_development_blob_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr};
  security::SecureBuffer blob;
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_blob_read, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout, &blob));
  return blob;
}

void write_vikey_development_blob_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& blob,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = &blob};
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_blob_write, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout));
}

security::SecureBuffer
read_vikey_development_card_record_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = nullptr,
      .expected_hardware_id = expected_hardware_id};
  security::SecureBuffer record;
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::card_record_read, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout, &record));
  return record;
}

void write_vikey_development_card_record_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_helper_digest,
    const std::filesystem::path& vendor_directory,
    const Sha256Digest& expected_vendor_digest,
    const std::uint32_t expected_hardware_id,
    const security::SecureBuffer& user_sn,
    const security::SecureBuffer& api_key,
    const security::SecureBuffer& user_pin,
    const security::SecureBuffer& record,
    const std::chrono::milliseconds timeout) {
  const std::filesystem::path vendor_path =
      vendor_directory / kVendorDllName;
  const DevelopmentSessionInputs inputs{
      .user_sn = user_sn,
      .api_key = api_key,
      .user_pin = user_pin,
      .blob = &record,
      .expected_hardware_id = expected_hardware_id};
  static_cast<void>(probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::card_record_write, &vendor_path,
      &expected_vendor_digest, &inputs, nullptr,
      HelperBoundary::network_enabled_development, timeout));
}

}  // namespace usb_vault::card::vikey
