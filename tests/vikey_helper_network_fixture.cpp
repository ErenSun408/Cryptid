#include "card/vikey/vikey_helper_protocol.h"
#include "vikey_network_probe.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using usb_vault::card::CardError;
using namespace usb_vault::card::vikey::helper;

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
    throw std::runtime_error("cannot inspect fixture environment");
  }
  std::wstring local_app_data;
  std::wstring temp;
  std::wstring tmp;
  std::wstring system_root;
  std::wstring windir;
  std::size_t count = 0;
  for (const wchar_t* entry = environment; *entry != L'\0';) {
    const std::wstring value(entry);
    entry += value.size() + 1;
    ++count;
    constexpr wchar_t kLocalAppData[] = L"LOCALAPPDATA=";
    constexpr wchar_t kTemp[] = L"TEMP=";
    constexpr wchar_t kTmp[] = L"TMP=";
    constexpr wchar_t kSystemRoot[] = L"SystemRoot=";
    constexpr wchar_t kWindowsDirectory[] = L"WINDIR=";
    if (value.starts_with(kLocalAppData)) {
      local_app_data = value.substr(std::size(kLocalAppData) - 1);
    } else if (value.starts_with(kTemp)) {
      temp = value.substr(std::size(kTemp) - 1);
    } else if (value.starts_with(kTmp)) {
      tmp = value.substr(std::size(kTmp) - 1);
    } else if (value.starts_with(kSystemRoot)) {
      system_root = value.substr(std::size(kSystemRoot) - 1);
    } else if (value.starts_with(kWindowsDirectory)) {
      windir = value.substr(std::size(kWindowsDirectory) - 1);
    } else {
      count = 0;
      break;
    }
  }
  if (::FreeEnvironmentStringsW(environment) == FALSE) {
    throw std::runtime_error("cannot release fixture environment");
  }
  const bool base_valid = !local_app_data.empty() && temp == tmp &&
                          temp == local_app_data + L"\\Temp";
  const bool isolated_environment =
      count == 3 && system_root.empty() && windir.empty();
  const bool development_environment =
      count == 5 && !system_root.empty() && windir == system_root;
  return base_valid &&
         (isolated_environment || development_environment);
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
      throw std::invalid_argument("invalid inherited fixture pipes");
    }
    const Request request = read_request(request_handle);
    Response response{};
    response.nonce = request.nonce;
    try {
      if (sanitized_environment()) {
        response.security_properties =
            property_bit(SecurityProperty::sanitized_environment);
      } else {
        response.status =
            static_cast<std::int32_t>(CardError::backend_unavailable);
      }
    } catch (...) {
      response.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
      response.security_properties = 0;
    }
    write_response(response_handle, response);
    if (response.status != 0) return 3;
    const std::uint32_t diagnostic =
        usb_vault::card::vikey::test::network_probe_diagnostic();
#if defined(USB_VAULT_EXPECT_NETWORK_ACCESS)
    return (diagnostic & 0xf0000000U) == 0x40000000U ? 0 : 4;
#else
    return static_cast<int>(diagnostic);
#endif
  } catch (...) {
    return 2;
  }
}
