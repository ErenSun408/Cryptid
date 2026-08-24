#include "card/vikey/vikey_helper_protocol.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <stdexcept>

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

[[nodiscard]] bool empty_environment() {
  wchar_t* const environment = ::GetEnvironmentStringsW();
  if (environment == nullptr) {
    throw std::runtime_error("cannot inspect helper environment");
  }
  const bool empty = *environment == L'\0';
  if (::FreeEnvironmentStringsW(environment) == FALSE) {
    throw std::runtime_error("cannot release helper environment");
  }
  return empty;
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
      if (empty_environment()) {
        response.security_properties =
            property_bit(SecurityProperty::empty_environment);
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
    return 0;
  } catch (...) {
    return 2;
  }
}
