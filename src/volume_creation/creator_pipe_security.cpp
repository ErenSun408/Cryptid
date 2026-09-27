#include "volume_creation/creator_pipe_security.h"
#include "usb_vault/diagnostics/debug_log.h"

#include <sddl.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace usb_vault::volume_creation::detail {
namespace {

class Handle final {
 public:
  explicit Handle(const HANDLE value = nullptr) noexcept : value_(value) {}
  ~Handle() noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(value_));
    }
  }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return value_; }

 private:
  HANDLE value_{};
};

class LocalString final {
 public:
  explicit LocalString(wchar_t* value = nullptr) noexcept : value_(value) {}
  ~LocalString() noexcept {
    if (value_ != nullptr) static_cast<void>(::LocalFree(value_));
  }

  LocalString(const LocalString&) = delete;
  LocalString& operator=(const LocalString&) = delete;

  [[nodiscard]] const wchar_t* get() const noexcept { return value_; }

 private:
  wchar_t* value_{};
};

}  // namespace

LocalSecurityDescriptor::LocalSecurityDescriptor(
    PSECURITY_DESCRIPTOR value) noexcept
    : value_(value) {}

LocalSecurityDescriptor::~LocalSecurityDescriptor() noexcept { reset(); }

LocalSecurityDescriptor::LocalSecurityDescriptor(
    LocalSecurityDescriptor&& other) noexcept
    : value_(std::exchange(other.value_, nullptr)) {}

LocalSecurityDescriptor& LocalSecurityDescriptor::operator=(
    LocalSecurityDescriptor&& other) noexcept {
  if (this != &other) {
    reset();
    value_ = std::exchange(other.value_, nullptr);
  }
  return *this;
}

PSECURITY_DESCRIPTOR LocalSecurityDescriptor::get() const noexcept {
  return value_;
}

void LocalSecurityDescriptor::reset() noexcept {
  if (value_ != nullptr) {
    static_cast<void>(::LocalFree(value_));
    value_ = nullptr;
  }
}

LocalSecurityDescriptor current_user_pipe_security_descriptor() noexcept {
  try {
    HANDLE raw_token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) ==
        FALSE) {
      return {};
    }
    const Handle token(raw_token);

    DWORD needed = 0;
    static_cast<void>(
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed));
    if (needed == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return {};
    }
    std::vector<std::byte> buffer(needed);
    if (::GetTokenInformation(token.get(), TokenUser, buffer.data(), needed,
                              &needed) == FALSE) {
      return {};
    }
    const auto* const token_user =
        reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (::IsValidSid(token_user->User.Sid) == FALSE) return {};

    wchar_t* raw_sid = nullptr;
    if (::ConvertSidToStringSidW(token_user->User.Sid, &raw_sid) == FALSE) {
      return {};
    }
    const LocalString sid_text(raw_sid);

    // ConvertSidToStringSidW emits only the canonical SID alphabet, so this
    // value cannot inject additional SDDL entries.
    const std::wstring definition =
        std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid_text.get() + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            definition.c_str(), SDDL_REVISION_1, &descriptor, nullptr) ==
            FALSE ||
        descriptor == nullptr) {
      if (descriptor != nullptr) static_cast<void>(::LocalFree(descriptor));
      return {};
    }
    return LocalSecurityDescriptor(descriptor);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("volume_creation/creator_pipe_security", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return {};
  }
}

}  // namespace usb_vault::volume_creation::detail
