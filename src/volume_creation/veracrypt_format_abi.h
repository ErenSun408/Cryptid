#pragma once

#include "usb_vault/volume_creation/volume_creator.h"

#include <Windows.h>

#include <cstdint>

namespace usb_vault::volume_creation::detail::format_abi {

using ProgressCallback = BOOL(CALLBACK*)(int percent_complete, void* user_data);

// ABI mirror of the pinned VeraCrypt 1.26.29 FormatDLL options structure plus
// the two hidden-volume fields from the reviewed usb-vault patch. The helper
// loads the DLL dynamically so the closed-source application does not link it.
struct Options final {
  const wchar_t* path;
  BOOL is_device;
  const char* password;
  const wchar_t** keyfiles;
  int pim;
  std::uint64_t size;
  const wchar_t* encryption_algorithm;
  const wchar_t* hash_algorithm;
  const wchar_t* filesystem;
  int cluster_size;
  BOOL quick_format;
  BOOL dynamic_format;
  BOOL fast_create_file;
  ProgressCallback progress_callback;
  void* progress_user_data;
  BOOL hidden_volume;
  std::uint64_t hidden_volume_host_size;
};

using Initialize = int(__cdecl*)();
using Shutdown = void(__cdecl*)();
using FormatBinary = int(__cdecl*)(const Options*, std::uint32_t);

constexpr int kSuccess = 0;
constexpr int kInvalidParameter = 2;
constexpr int kPasswordPolicy = 7;
constexpr int kOsError = 10;
constexpr int kUserAbort = 16;
constexpr int kAccessDenied = 23;

[[nodiscard]] constexpr CreationError map_error(const int value) noexcept {
  switch (value) {
    case kInvalidParameter:
    case kPasswordPolicy:
      return CreationError::invalid_request;
    case kUserAbort:
      return CreationError::operation_cancelled;
    case kAccessDenied:
      return CreationError::target_access_denied;
    case kOsError:
      return CreationError::io_error;
    default:
      return CreationError::format_rejected;
  }
}

}  // namespace usb_vault::volume_creation::detail::format_abi
