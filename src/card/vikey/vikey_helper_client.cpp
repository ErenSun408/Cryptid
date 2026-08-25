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

class AttributeList final {
 public:
  explicit AttributeList(const DWORD attribute_count) {
    SIZE_T bytes = 0;
    static_cast<void>(
        ::InitializeProcThreadAttributeList(nullptr, attribute_count, 0,
                                            &bytes));
    if (bytes == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_backend("size ViKey helper process attributes");
    }
    storage_.resize(bytes);
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    if (::InitializeProcThreadAttributeList(list_, attribute_count, 0,
                                            &bytes) == FALSE) {
      throw_backend("initialize ViKey helper process attributes");
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
           const SIZE_T bytes) {
    if (::UpdateProcThreadAttribute(list_, 0, attribute, value, bytes,
                                    nullptr, nullptr) == FALSE) {
      throw_backend("set ViKey helper process attribute");
    }
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept {
    return list_;
  }

 private:
  [[noreturn]] static void throw_backend(const char* const context) {
    throw std::system_error(make_error_code(CardError::backend_unavailable),
                            context);
  }

  std::vector<std::byte> storage_;
  LPPROC_THREAD_ATTRIBUTE_LIST list_{};
};

[[noreturn]] void throw_card(const CardError error,
                             const char* const context) {
  throw std::system_error(make_error_code(error), context);
}

[[nodiscard]] Pipe create_pipe(const bool child_reads) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read = INVALID_HANDLE_VALUE;
  HANDLE write = INVALID_HANDLE_VALUE;
  if (::CreatePipe(&read, &write, &security, 0) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "create ViKey helper IPC pipe");
  }
  Handle read_handle(read);
  Handle write_handle(write);
  HANDLE parent = child_reads ? write_handle.get() : read_handle.get();
  if (::SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "make ViKey parent pipe handle non-inheritable");
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
    throw_card(CardError::backend_unavailable,
               "duplicate ViKey helper IPC handle");
  }
  return Handle(duplicate);
}

[[nodiscard]] Handle create_restricted_token() {
  HANDLE raw_token = INVALID_HANDLE_VALUE;
  if (::OpenProcessToken(::GetCurrentProcess(),
                         TOKEN_DUPLICATE | TOKEN_QUERY |
                             TOKEN_ASSIGN_PRIMARY | TOKEN_IMPERSONATE,
                         &raw_token) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "open current token for ViKey helper");
  }
  Handle source_token(raw_token);

  DWORD restricted_bytes = 0;
  static_cast<void>(::GetTokenInformation(source_token.get(),
                                          TokenRestrictedSids, nullptr, 0,
                                          &restricted_bytes));
  if (restricted_bytes == 0 ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_card(CardError::backend_unavailable,
               "size current token restrictions for ViKey helper");
  }
  std::vector<std::byte> restricted_storage(restricted_bytes);
  if (::GetTokenInformation(source_token.get(), TokenRestrictedSids,
                            restricted_storage.data(), restricted_bytes,
                            &restricted_bytes) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "read current token restrictions for ViKey helper");
  }
  const auto* const existing_restrictions =
      reinterpret_cast<const TOKEN_GROUPS*>(restricted_storage.data());
  if (existing_restrictions->GroupCount != 0) {
    throw_card(CardError::backend_unavailable,
               "ViKey helper cannot safely refilter an already restricted "
               "parent token");
  }

  HANDLE raw_restricted = INVALID_HANDLE_VALUE;
  if (::CreateRestrictedToken(source_token.get(), DISABLE_MAX_PRIVILEGE, 0,
                              nullptr, 0, nullptr, 0, nullptr,
                              &raw_restricted) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "create restricted ViKey helper token");
  }
  return Handle(raw_restricted);
}

void filter_development_process_privileges(const HANDLE token) {
  LUID change_notify{};
  if (::LookupPrivilegeValueW(nullptr, L"SeChangeNotifyPrivilege",
                              &change_notify) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "resolve development ViKey helper traversal privilege");
  }

  DWORD privilege_bytes = 0;
  static_cast<void>(
      ::GetTokenInformation(token, TokenPrivileges, nullptr, 0,
                            &privilege_bytes));
  if (privilege_bytes < sizeof(TOKEN_PRIVILEGES) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_card(CardError::backend_unavailable,
               "size development ViKey helper token privileges");
  }
  std::vector<std::byte> privilege_storage(privilege_bytes);
  if (::GetTokenInformation(token, TokenPrivileges, privilege_storage.data(),
                            privilege_bytes, &privilege_bytes) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "read development ViKey helper token privileges");
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
    throw_card(CardError::backend_unavailable,
               "filter development ViKey helper token privileges");
  }
}

[[nodiscard]] Handle create_single_process_job() {
  Handle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job) {
    throw_card(CardError::backend_unavailable,
               "create ViKey helper job object");
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  if (::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "limit ViKey helper job object");
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
    throw_card(CardError::backend_unavailable,
               "resolve ViKey helper traversal privilege");
  }

  DWORD returned = 0;
  static_cast<void>(
      ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &returned));
  if (returned < sizeof(TOKEN_PRIVILEGES) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    throw_card(CardError::backend_unavailable,
               "size ViKey helper token privileges");
  }
  std::vector<std::byte> privilege_storage(returned);
  if (::GetTokenInformation(token, TokenPrivileges, privilege_storage.data(),
                            returned, &returned) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "inspect ViKey helper token privileges");
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
      throw_card(CardError::backend_unavailable,
                 "development ViKey helper retained a removable privilege");
    }
    if (enabled && !is_change_notify) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper token retained an enabled privilege");
    }
  }
  properties |=
      helper::property_bit(helper::SecurityProperty::filtered_token);

  TOKEN_ELEVATION elevation{};
  returned = 0;
  if (::GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &returned) == FALSE ||
      returned != sizeof(elevation)) {
    throw_card(CardError::backend_unavailable,
               "inspect ViKey helper token elevation");
  }
  if (elevation.TokenIsElevated == 0) {
    properties |=
        helper::property_bit(helper::SecurityProperty::not_elevated);
  }

  BOOL is_app_container = FALSE;
  returned = 0;
  if (::GetTokenInformation(token, TokenIsAppContainer, &is_app_container,
                            sizeof(is_app_container), &returned) == FALSE ||
      returned != sizeof(is_app_container)) {
    throw_card(CardError::backend_unavailable,
               "inspect ViKey helper AppContainer state");
  }
  if (expected_app_container_sid == nullptr) {
    if (is_app_container != FALSE) {
      throw_card(CardError::backend_unavailable,
                 "development ViKey helper unexpectedly entered an "
                 "AppContainer");
    }
  } else {
    if (is_app_container == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper is not in an AppContainer");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::app_container);

    DWORD app_container_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenAppContainerSid, nullptr, 0, &app_container_bytes));
    if (app_container_bytes < sizeof(TOKEN_APPCONTAINER_INFORMATION) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_card(CardError::backend_unavailable,
                 "size ViKey helper AppContainer identity");
    }
    std::vector<std::byte> app_container_storage(app_container_bytes);
    if (::GetTokenInformation(token, TokenAppContainerSid,
                              app_container_storage.data(),
                              app_container_bytes,
                              &app_container_bytes) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "inspect ViKey helper AppContainer identity");
    }
    const auto* const app_container =
        reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(
            app_container_storage.data());
    if (app_container->TokenAppContainer == nullptr ||
        ::EqualSid(app_container->TokenAppContainer,
                   expected_app_container_sid) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper AppContainer identity is invalid");
    }

    DWORD capability_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenCapabilities, nullptr, 0, &capability_bytes));
    if (capability_bytes < sizeof(DWORD) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_card(CardError::backend_unavailable,
                 "size ViKey helper capabilities");
    }
    std::vector<std::byte> capability_storage(capability_bytes);
    if (::GetTokenInformation(token, TokenCapabilities,
                              capability_storage.data(), capability_bytes,
                              &capability_bytes) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "inspect ViKey helper capabilities");
    }
    const auto* const capabilities =
        reinterpret_cast<const TOKEN_GROUPS*>(capability_storage.data());
    if (capabilities->GroupCount != 0) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper retained an AppContainer capability");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::no_capabilities);

    DWORD integrity_bytes = 0;
    static_cast<void>(::GetTokenInformation(
        token, TokenIntegrityLevel, nullptr, 0, &integrity_bytes));
    if (integrity_bytes < sizeof(TOKEN_MANDATORY_LABEL) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      throw_card(CardError::backend_unavailable,
                 "size ViKey helper integrity level");
    }
    std::vector<std::byte> integrity_storage(integrity_bytes);
    if (::GetTokenInformation(token, TokenIntegrityLevel,
                              integrity_storage.data(), integrity_bytes,
                              &integrity_bytes) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "inspect ViKey helper integrity level");
    }
    const auto* const integrity =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(
            integrity_storage.data());
    if (integrity->Label.Sid == nullptr ||
        ::IsValidSid(integrity->Label.Sid) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper integrity SID is invalid");
    }
    const UCHAR subauthority_count =
        *::GetSidSubAuthorityCount(integrity->Label.Sid);
    if (subauthority_count == 0 ||
        *::GetSidSubAuthority(integrity->Label.Sid,
                              subauthority_count - 1) >
            SECURITY_MANDATORY_LOW_RID) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper integrity level is too high");
    }
    properties |=
        helper::property_bit(helper::SecurityProperty::low_integrity);
  }

  BOOL in_job = FALSE;
  if (::IsProcessInJob(process, job, &in_job) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "inspect ViKey helper job membership");
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
  if (::GetProcessMitigationPolicy(process, ProcessChildProcessPolicy, &child,
                                   sizeof(child)) == FALSE ||
      ::GetProcessMitigationPolicy(process, ProcessDEPPolicy, &dep,
                                   sizeof(dep)) == FALSE ||
      ::GetProcessMitigationPolicy(process, ProcessASLRPolicy, &aslr,
                                   sizeof(aslr)) == FALSE ||
      ::GetProcessMitigationPolicy(process, ProcessStrictHandleCheckPolicy,
                                   &handles, sizeof(handles)) == FALSE ||
      ::GetProcessMitigationPolicy(process,
                                   ProcessExtensionPointDisablePolicy,
                                   &extensions, sizeof(extensions)) == FALSE ||
      ::GetProcessMitigationPolicy(process, ProcessImageLoadPolicy, &images,
                                   sizeof(images)) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "inspect ViKey helper process mitigations");
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
      throw std::system_error(
          make_error_code(CardError::backend_unavailable),
          std::format("remove ephemeral ViKey helper AppContainer profile "
                      "(HRESULT 0x{:08X})",
                      static_cast<std::uint32_t>(result)));
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
    throw_card(CardError::backend_unavailable,
               "create ephemeral ViKey helper AppContainer profile");
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
  if (::ConvertSidToStringSidW(app_container_sid, &sid_string) == FALSE ||
      sid_string == nullptr) {
    throw_card(CardError::backend_unavailable,
               "format ViKey helper AppContainer identity");
  }
  wchar_t* folder = nullptr;
  const HRESULT folder_result =
      ::GetAppContainerFolderPath(sid_string, &folder);
  static_cast<void>(::LocalFree(sid_string));
  if (FAILED(folder_result) || folder == nullptr) {
    if (folder != nullptr) ::CoTaskMemFree(folder);
    throw_card(CardError::backend_unavailable,
               "resolve ViKey helper AppContainer folder");
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
    throw_card(CardError::backend_unavailable,
               "resolve AppContainer virtual LocalAppData path");
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
      throw std::system_error(
          make_error_code(CardError::backend_unavailable),
          std::format("remove development ViKey helper directory "
                      "(Windows error {})",
                      error));
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
    throw_card(CardError::backend_unavailable,
               "resolve development ViKey helper LocalAppData path");
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
      throw_card(CardError::backend_unavailable,
                 "create development ViKey helper directory");
    }
    const std::filesystem::path temp = root / L"Temp";
    if (::CreateDirectoryW(temp.c_str(), nullptr) == FALSE) {
      const DWORD error = ::GetLastError();
      if (::RemoveDirectoryW(root.c_str()) == FALSE) {
        throw_card(CardError::backend_unavailable,
                   "remove incomplete development ViKey helper directory");
      }
      throw std::system_error(
          make_error_code(CardError::backend_unavailable),
          std::format("create development ViKey helper Temp directory "
                      "(Windows error {})",
                      error));
    }
    return std::make_unique<EphemeralDevelopmentDirectory>(root);
  }
  throw_card(CardError::backend_unavailable,
             "allocate unique development ViKey helper directory");
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
    throw std::system_error(
        make_error_code(CardError::backend_unavailable),
        std::format("stage verified ViKey DLL (Windows error {})",
                    ::GetLastError()));
  }

  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"S:(ML;;NW;;;ME)", SDDL_REVISION_1, &raw_descriptor,
          nullptr) == FALSE ||
      raw_descriptor == nullptr) {
    if (raw_descriptor != nullptr) ::LocalFree(raw_descriptor);
    throw std::system_error(
        make_error_code(CardError::backend_unavailable),
        "create medium-integrity label for staged ViKey DLL");
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
    throw std::system_error(
        make_error_code(CardError::backend_unavailable),
        std::format("label staged ViKey DLL as medium integrity "
                    "(Windows error {})",
                    label_result));
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
      throw std::system_error(
          make_error_code(CardError::backend_unavailable),
          std::format("remove staged ViKey DLL (Windows error {})", error));
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
  // Always attempt profile removal even if the staged file cleanup failed.
  // A profile cleanup failure is the more persistent condition and therefore
  // supersedes a staged-file error.
  app_container.remove_profile();
  if (staged_failure) std::rethrow_exception(staged_failure);
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
  directory.remove_directories();
  if (staged_failure) std::rethrow_exception(staged_failure);
}

[[nodiscard]] helper::Response read_response_until(
    const HANDLE pipe, const HANDLE process,
    const std::array<std::byte, helper::kNonceSize>& nonce,
    const std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    DWORD available = 0;
    if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ==
        FALSE) {
      throw_card(CardError::communication_failed,
                 "inspect ViKey helper IPC response");
    }
    if (available >= sizeof(helper::Response)) {
      return helper::read_response(pipe, nonce);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw_card(CardError::communication_failed,
                 "ViKey helper IPC response timed out");
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const DWORD wait_milliseconds = static_cast<DWORD>(
        std::clamp<std::int64_t>(remaining.count(), 1, 10));
    const DWORD wait = ::WaitForSingleObject(process, wait_milliseconds);
    if (wait == WAIT_FAILED) {
      throw_card(CardError::communication_failed,
                 "wait for ViKey helper response");
    }
    if (wait == WAIT_OBJECT_0) {
      if (::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) !=
              FALSE &&
          available >= sizeof(helper::Response)) {
        return helper::read_response(pipe, nonce);
      }
      DWORD exit_code = 0;
      if (::GetExitCodeProcess(process, &exit_code) != FALSE) {
        throw std::system_error(
            make_error_code(CardError::communication_failed),
            std::format("ViKey helper exited without a complete response "
                        "(exit code 0x{:08X})",
                        exit_code));
      }
      throw_card(CardError::communication_failed,
                 "ViKey helper exited without a complete response");
    }
  }
}

void require_clean_exit(const HANDLE process, const HANDLE response_pipe,
                        const std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    throw_card(CardError::communication_failed,
               "ViKey helper did not exit before its deadline");
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - now);
  const auto bounded = std::clamp<std::int64_t>(
      remaining.count(), 1, std::numeric_limits<DWORD>::max());
  if (::WaitForSingleObject(process, static_cast<DWORD>(bounded)) !=
      WAIT_OBJECT_0) {
    throw_card(CardError::communication_failed,
               "ViKey helper did not exit cleanly");
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(process, &exit_code) == FALSE) {
    throw_card(CardError::communication_failed,
               "read ViKey helper exit code");
  }
  if (exit_code != 0) {
    throw std::system_error(
        make_error_code(CardError::communication_failed),
        std::format("ViKey helper returned failure exit code 0x{:08X}",
                    exit_code));
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
    const HelperBoundary boundary,
    const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) {
    throw_card(CardError::invalid_configuration,
               "ViKey helper timeout must be positive");
  }
  const bool session_required =
      operation == helper::Operation::development_session_probe;
  const bool vendor_required =
      operation == helper::Operation::vendor_runtime_probe ||
      session_required;
  if (vendor_required !=
      (vendor_path != nullptr && vendor_digest != nullptr)) {
    throw_card(CardError::invalid_configuration,
               "ViKey vendor probe configuration is incomplete");
  }
  if (session_required != (session_inputs != nullptr) ||
      (session_required &&
       boundary != HelperBoundary::network_enabled_development)) {
    throw_card(CardError::invalid_configuration,
               "ViKey development session configuration is incomplete");
  }
  if (session_inputs != nullptr &&
      (session_inputs->user_sn.size() != kVikeyUserSnBytes ||
       session_inputs->api_key.size() != kVikeyApiKeyBytes ||
       session_inputs->user_pin.size() != kVikeyUserPinBytes)) {
    throw_card(CardError::invalid_configuration,
               "ViKey development session credential size is invalid");
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
                 sizeof(inherited_handles));
  attributes.add(PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
                 sizeof(mitigations));
  attributes.add(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &child_policy,
                 sizeof(child_policy));

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
                     &security_capabilities, sizeof(security_capabilities));
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
    cleanup_boundary();
    throw;
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
    // behind when the verified artifact cannot be started. Cleanup failure is
    // more urgent than the launch error and therefore supersedes it.
    cleanup_boundary();
    throw std::system_error(
        make_error_code(CardError::backend_unavailable),
        std::format("launch restricted ViKey helper (Windows error {})",
                    error));
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
      throw_card(CardError::backend_unavailable,
                 "open ViKey helper process token");
    }
    Handle process_token(raw_process_token);

    if (boundary == HelperBoundary::network_enabled_development) {
      // The primary thread is still suspended. Remove every privilege except
      // directory traversal before any helper or vendor instruction executes.
      filter_development_process_privileges(process_token.get());
    }

    if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
      throw_card(CardError::backend_unavailable,
                 "assign ViKey helper to job object");
    }
    parent_security_properties =
        inspect_process_security(process_token.get(), process.get(), job.get(),
                                 app_container ? app_container->sid.get()
                                               : nullptr);
    if (parent_security_properties !=
        parent_required_security_properties) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper security boundary is incomplete");
    }
    if (::ResumeThread(thread.get()) == std::numeric_limits<DWORD>::max()) {
      throw_card(CardError::backend_unavailable,
                 "resume restricted ViKey helper");
    }
    thread.reset();

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    helper::Request request{};
    request.operation = operation;
    request.nonce = random_nonce();
    if (session_inputs != nullptr && vendor_digest != nullptr) {
      request.payload_bytes = helper::kDevelopmentSessionPayloadBytes;
      helper::write_development_session_request(
          request_pipe.parent_end.get(), request, *vendor_digest,
          session_inputs->user_sn, session_inputs->api_key,
          session_inputs->user_pin);
    } else {
      helper::write_request(request_pipe.parent_end.get(), request);
    }
    request_pipe.parent_end.reset();

    const helper::Response response = read_response_until(
        response_pipe.parent_end.get(), process.get(), request.nonce,
        deadline);
    if (response.status != 0) {
      throw std::system_error(
          make_error_code(static_cast<CardError>(response.status)),
          std::format("ViKey helper rejected the requested operation "
                      "(detail {})",
                      response.detail));
    }
    const std::uint32_t security_properties =
        parent_security_properties | response.security_properties;
    if (security_properties != required_security_properties) {
      throw_card(CardError::backend_unavailable,
                 "ViKey helper security boundary is incomplete");
    }
    require_clean_exit(process.get(), response_pipe.parent_end.get(),
                       deadline);
    result.security_properties = security_properties;
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    static_cast<void>(::TerminateProcess(process.get(), 1));
    static_cast<void>(::WaitForSingleObject(process.get(), 1000));
    // Ephemeral launch resources are removed on every observed failure path.
    // Cleanup failure supersedes the original error so persistent residue
    // cannot go unnoticed.
    cleanup_boundary();
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
      nullptr, nullptr, HelperBoundary::zero_network_app_container, timeout);
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
      &expected_vendor_digest, nullptr,
      HelperBoundary::zero_network_app_container, timeout);
}

VikeyHelperProbe probe_vikey_hid_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::hid_access_probe,
      nullptr, nullptr, nullptr, HelperBoundary::zero_network_app_container,
      timeout);
}

VikeyHelperProbe probe_vikey_development_helper_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::probe, nullptr,
      nullptr, nullptr, HelperBoundary::network_enabled_development, timeout);
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
      &expected_vendor_digest, nullptr,
      HelperBoundary::network_enabled_development, timeout);
}

VikeyHelperProbe
probe_vikey_development_hid_access_with_network_access(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  return probe_vikey_helper_operation(
      helper_path, expected_digest, helper::Operation::hid_access_probe,
      nullptr, nullptr, nullptr, HelperBoundary::network_enabled_development,
      timeout);
}

VikeyHelperProbe probe_vikey_development_session_with_network_access(
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
      .user_sn = user_sn, .api_key = api_key, .user_pin = user_pin};
  return probe_vikey_helper_operation(
      helper_path, expected_helper_digest,
      helper::Operation::development_session_probe, &vendor_path,
      &expected_vendor_digest, &inputs,
      HelperBoundary::network_enabled_development, timeout);
}

}  // namespace usb_vault::card::vikey
