#include "usb_vault/volume/fat32_file_transfer.h"
#include <string>
#include <system_error>
#include <utility>
namespace usb_vault::volume {
Fat32TransferTarget::Fat32TransferTarget(std::wstring_view path) {
  if (path.empty() || path.find(L'\0') != std::wstring_view::npos)
    throw std::invalid_argument("invalid FAT32 target path");
  const std::wstring owned(path);
  file_ = ::CreateFileW(owned.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0,
      nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file_ == INVALID_HANDLE_VALUE)
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "create FAT32 transfer target");
}
Fat32TransferTarget::~Fat32TransferTarget() noexcept { static_cast<void>(close()); }
DWORD Fat32TransferTarget::close() noexcept {
  if (file_ == INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
  const auto file = std::exchange(file_, INVALID_HANDLE_VALUE);
  return ::CloseHandle(file) ? ERROR_SUCCESS : ::GetLastError();
}
Fat32TransferResult transfer_fat32_file(HANDLE source, Fat32TransferTarget& target,
    const MountIdentity& expected, std::uint64_t boundary, const Fat32AllocationPlan& plan,
    const std::function<bool()>& cancelled, const std::function<void(std::uint64_t)>& progress) {
  if (target.file_ == INVALID_HANDLE_VALUE)
    throw std::logic_error("FAT32 transfer target already consumed");
  Fat32TransferResult result;
  try {
    prepare_fat32_file(target.file_, expected, boundary, plan, cancelled);
    write_prepared_fat32_file(source, target.file_, expected, boundary, plan, cancelled, progress);
  } catch (...) {
    result.failure = std::current_exception();
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (::SetFileInformationByHandle(target.file_, FileDispositionInfo, &disposition, sizeof(disposition)))
      result.deletion_requested = true;
    else result.cleanup_error = ::GetLastError();
  }
  const auto close_error = target.close();
  if (close_error != ERROR_SUCCESS) {
    if (result.failure) {
      if (result.cleanup_error == ERROR_SUCCESS) result.cleanup_error = close_error;
    } else {
      result.failure = std::make_exception_ptr(std::system_error(static_cast<int>(close_error), std::system_category(), "close FAT32 transfer target"));
    }
  }
  return result;
}
}
