#include "card/vikey/vikey_helper_protocol.h"
#include "card/vikey/vikey_hid_probe.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using usb_vault::card::CardError;
using namespace usb_vault::card::vikey::helper;

class Module final {
 public:
  explicit Module(const HMODULE value) noexcept : value_(value) {}
  ~Module() noexcept {
    if (value_ != nullptr) static_cast<void>(::FreeLibrary(value_));
  }
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  [[nodiscard]] HMODULE get() const noexcept { return value_; }

 private:
  HMODULE value_{};
};

class File final {
 public:
  explicit File(const HANDLE value) noexcept : value_(value) {}
  ~File() noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::CloseHandle(value_));
    }
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] HANDLE inherited_pipe(const DWORD standard_handle) {
  const HANDLE handle = ::GetStdHandle(standard_handle);
  DWORD flags = 0;
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      ::GetHandleInformation(handle, &flags) == FALSE ||
      ::GetFileType(handle) != FILE_TYPE_PIPE) {
    throw std::invalid_argument("inherited handle is not a pipe");
  }
  return handle;
}

[[nodiscard]] bool sanitized_environment() {
  wchar_t* const environment = ::GetEnvironmentStringsW();
  if (environment == nullptr) {
    throw std::runtime_error("cannot inspect helper environment");
  }
  std::wstring local_app_data;
  std::wstring temp;
  std::wstring tmp;
  std::size_t count = 0;
  for (const wchar_t* entry = environment; *entry != L'\0';) {
    const std::wstring value(entry);
    entry += value.size() + 1;
    ++count;
    constexpr wchar_t kLocalAppData[] = L"LOCALAPPDATA=";
    constexpr wchar_t kTemp[] = L"TEMP=";
    constexpr wchar_t kTmp[] = L"TMP=";
    if (value.starts_with(kLocalAppData)) {
      local_app_data = value.substr(std::size(kLocalAppData) - 1);
    } else if (value.starts_with(kTemp)) {
      temp = value.substr(std::size(kTemp) - 1);
    } else if (value.starts_with(kTmp)) {
      tmp = value.substr(std::size(kTmp) - 1);
    } else {
      count = 0;
      break;
    }
  }
  if (::FreeEnvironmentStringsW(environment) == FALSE) {
    throw std::runtime_error("cannot release helper environment");
  }
  return count == 3 && !local_app_data.empty() && temp == tmp &&
         temp == local_app_data + L"\\Temp";
}

[[nodiscard]] std::filesystem::path staged_vendor_dll() {
  const DWORD required =
      ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required <= 1) {
    throw std::runtime_error("cannot size helper private folder");
  }
  std::wstring folder(required, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(
      L"LOCALAPPDATA", folder.data(), required);
  if (written == 0 || written >= required) {
    throw std::runtime_error("cannot read helper private folder");
  }
  folder.resize(written);
  return std::filesystem::path(folder) / L"ViKey.dll";
}

struct OperationResult final {
  std::int32_t status{};
  std::uint32_t detail{};
};

[[nodiscard]] OperationResult vendor_runtime_compatibility() {
  const std::filesystem::path staged = staged_vendor_dll();
  File readable(::CreateFileW(staged.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!readable) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = ::GetLastError()};
  }
  if (::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) == FALSE) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = ::GetLastError()};
  }
  Module module(::LoadLibraryExW(staged.c_str(), nullptr, 0));
  if (module.get() == nullptr) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = ::GetLastError()};
  }

  using GetLibraryVersion = DWORD(__stdcall*)(char*);
  const auto get_library_version = reinterpret_cast<GetLibraryVersion>(
      ::GetProcAddress(module.get(), "VikeyGetLibraryVersion"));
  if (get_library_version == nullptr) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = ERROR_PROC_NOT_FOUND};
  }
  std::array<char, 64> version{};
  const DWORD vendor_status = get_library_version(version.data());
  if (vendor_status != ERROR_SUCCESS) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = vendor_status};
  }
  if (version.front() == '\0' ||
      std::find(version.begin(), version.end(), '\0') == version.end()) {
    return {.status = static_cast<std::int32_t>(
                CardError::backend_unavailable),
            .detail = ERROR_INVALID_DATA};
  }
  return {};
}

[[nodiscard]] OperationResult operation_result(const Operation operation) {
  switch (operation) {
    case Operation::probe:
      return {};
    case Operation::vendor_runtime_probe:
      return vendor_runtime_compatibility();
    case Operation::hid_access_probe: {
      const auto probe =
          usb_vault::card::vikey::probe_vikey_hid_access();
      if (!probe.device_present) {
        return {.status = static_cast<std::int32_t>(
                    CardError::device_not_found),
                .detail = ERROR_DEVICE_NOT_CONNECTED};
      }
      return probe.read_write_opened
                 ? OperationResult{}
                 : OperationResult{
                       .status = static_cast<std::int32_t>(
                           probe.open_error == ERROR_ACCESS_DENIED
                               ? CardError::permission_denied
                               : CardError::device_failure),
                       .detail = probe.open_error};
    }
  }
  return {.status =
              static_cast<std::int32_t>(CardError::invalid_operation),
          .detail = ERROR_INVALID_FUNCTION};
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR,
                    _In_ int) {
  try {
    const HANDLE request_handle = inherited_pipe(STD_INPUT_HANDLE);
    const HANDLE response_handle = inherited_pipe(STD_OUTPUT_HANDLE);
    const HANDLE error_handle = inherited_pipe(STD_ERROR_HANDLE);
    if (request_handle == response_handle || request_handle == error_handle ||
        response_handle == error_handle) {
      throw std::invalid_argument("invalid inherited ViKey helper pipes");
    }
    // These are the process standard handles. Leave them valid through CRT
    // teardown and let process termination reclaim them; closing them here can
    // make an optimized static CRT touch already-closed standard streams.
    const Request request = read_request(request_handle);
    Response response{};
    response.nonce = request.nonce;
    try {
      if (sanitized_environment()) {
        const OperationResult result = operation_result(request.operation);
        response.status = result.status;
        response.detail = result.detail;
      } else {
        response.status =
            static_cast<std::int32_t>(CardError::backend_unavailable);
        response.detail = ERROR_BAD_ENVIRONMENT;
      }
      if (response.status == 0) {
        response.security_properties =
            property_bit(SecurityProperty::sanitized_environment);
      }
    } catch (...) {
      response.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
      response.security_properties = 0;
      response.detail = ERROR_GEN_FAILURE;
    }
    write_response(response_handle, response);
    return 0;
  } catch (...) {
    return 2;
  }
}
