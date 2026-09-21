#pragma once
#include "usb_vault/application/card_operation_lease.h"
#include <Windows.h>
#include <system_error>

namespace usb_vault::card::vikey {
// Fixed neutral name: no hardware identity or user material in kernel names.
// Recursive for nested helper calls on the owning transaction thread. No wait:
// an unrelated instance must not silently enqueue a destructive action.
class CardOperationLease final : public application::ICardOperationLease {
 public:
  explicit CardOperationLease(const wchar_t* name = L"Global\\USBVault.CardOperation.v1") {
    handle_ = ::CreateMutexW(nullptr, FALSE, name);
    if (!handle_) throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "card operation mutex");
    const DWORD result = ::WaitForSingleObject(handle_, 0);
    if (result != WAIT_OBJECT_0) {
      const DWORD error = result == WAIT_FAILED ? ::GetLastError() : ERROR_BUSY;
      if (result == WAIT_ABANDONED) ::ReleaseMutex(handle_);
      ::CloseHandle(handle_);
      handle_ = nullptr;
      throw std::system_error(static_cast<int>(error), std::system_category(), "card operation unavailable");
    }
  }
  ~CardOperationLease() noexcept override {
    if (handle_) { ::ReleaseMutex(handle_); ::CloseHandle(handle_); }
  }
  CardOperationLease(const CardOperationLease&) = delete;
  CardOperationLease& operator=(const CardOperationLease&) = delete;
 private:
  HANDLE handle_{};
};
}  // namespace usb_vault::card::vikey
