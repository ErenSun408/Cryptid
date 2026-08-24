#include "card/vikey/vikey_helper_client.h"

#include "card/vikey/vikey_helper_protocol.h"
#include "usb_vault/card/card_error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::card::vikey {
namespace {

constexpr std::uint64_t kMaximumHelperBytes = 16ULL * 1024ULL * 1024ULL;

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
    const HANDLE token, const HANDLE process, const HANDLE job) {
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

VikeyHelperProbe probe_vikey_helper(
    const std::filesystem::path& helper_path,
    const Sha256Digest& expected_digest,
    const std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero()) {
    throw_card(CardError::invalid_configuration,
               "ViKey helper timeout must be positive");
  }
  VerifiedLocalArtifact executable_artifact = verify_local_artifact(
      helper_path, expected_digest, kMaximumHelperBytes);
  const std::filesystem::path& executable = executable_artifact.path();
  Pipe request_pipe = create_pipe(true);
  Pipe response_pipe = create_pipe(false);
  Handle error_pipe =
      duplicate_inheritable_handle(response_pipe.child_end.get());
  Handle token = create_restricted_token();
  Handle job = create_single_process_job();

  std::array<HANDLE, 3> inherited_handles{
      request_pipe.child_end.get(), response_pipe.child_end.get(),
      error_pipe.get()};
  DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
  DWORD64 mitigations = kHelperMitigations;
  AttributeList attributes(3);
  attributes.add(PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited_handles.data(),
                 sizeof(inherited_handles));
  attributes.add(PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &mitigations,
                 sizeof(mitigations));
  attributes.add(PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &child_policy,
                 sizeof(child_policy));

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = request_pipe.child_end.get();
  startup.StartupInfo.hStdOutput = response_pipe.child_end.get();
  startup.StartupInfo.hStdError = error_pipe.get();
  startup.lpAttributeList = attributes.get();
  PROCESS_INFORMATION process_info{};
  std::wstring command = std::format(L"\"{}\"", executable.native());
  std::array<wchar_t, 2> empty_environment{};
  constexpr DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT |
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW |
                                   CREATE_UNICODE_ENVIRONMENT;
  if (::CreateProcessAsUserW(
          token.get(), executable.c_str(), command.data(), nullptr, nullptr,
          TRUE, creation_flags, empty_environment.data(), nullptr,
          &startup.StartupInfo, &process_info) == FALSE) {
    throw_card(CardError::backend_unavailable,
               "launch restricted ViKey helper");
  }
  Handle process(process_info.hProcess);
  Handle thread(process_info.hThread);
  request_pipe.child_end.reset();
  response_pipe.child_end.reset();
  error_pipe.reset();

  if (::AssignProcessToJobObject(job.get(), process.get()) == FALSE) {
    static_cast<void>(::TerminateProcess(process.get(), 1));
    throw_card(CardError::backend_unavailable,
               "assign ViKey helper to job object");
  }
  constexpr std::uint32_t kParentRequiredSecurityProperties =
      helper::kRequiredSecurityProperties &
      ~helper::property_bit(helper::SecurityProperty::empty_environment);
  std::uint32_t parent_security_properties = 0;
  try {
    parent_security_properties =
        inspect_process_security(token.get(), process.get(), job.get());
  } catch (...) {
    static_cast<void>(::TerminateProcess(process.get(), 1));
    throw;
  }
  if ((parent_security_properties & kParentRequiredSecurityProperties) !=
      kParentRequiredSecurityProperties) {
    static_cast<void>(::TerminateProcess(process.get(), 1));
    throw_card(CardError::backend_unavailable,
               "ViKey helper security boundary is incomplete");
  }
  if (::ResumeThread(thread.get()) == std::numeric_limits<DWORD>::max()) {
    static_cast<void>(::TerminateProcess(process.get(), 1));
    throw_card(CardError::backend_unavailable,
               "resume restricted ViKey helper");
  }
  thread.reset();

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  helper::Request request{};
  request.nonce = random_nonce();
  helper::write_request(request_pipe.parent_end.get(), request);
  request_pipe.parent_end.reset();

  const helper::Response response = read_response_until(
      response_pipe.parent_end.get(), process.get(), request.nonce, deadline);
  if (response.status != 0) {
    throw_card(static_cast<CardError>(response.status),
               "ViKey helper rejected its security boundary");
  }
  const std::uint32_t security_properties =
      parent_security_properties | response.security_properties;
  if ((security_properties & helper::kRequiredSecurityProperties) !=
      helper::kRequiredSecurityProperties) {
    throw_card(CardError::backend_unavailable,
               "ViKey helper security boundary is incomplete");
  }
  require_clean_exit(process.get(), response_pipe.parent_end.get(), deadline);
  return {.security_properties = security_properties};
}

}  // namespace usb_vault::card::vikey
