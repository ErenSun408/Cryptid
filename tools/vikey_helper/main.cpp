#include "card/vikey/vikey_helper_protocol.h"
#include "card/vikey/vikey_hid_probe.h"
#include "card/vikey/vikey_artifact.h"
#include "card/vikey/vikey_blob_round_trip.h"
#include "card/vikey/vikey_card_record_io.h"
#include "card/vikey/vikey_module.h"
#include "card/vikey/vikey_session.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::card_error_category;
using namespace usb_vault::card::vikey::helper;

enum class EnvironmentBoundary {
  invalid,
  zero_network_app_container,
  network_enabled_development,
};

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

[[nodiscard]] EnvironmentBoundary sanitized_environment() {
  wchar_t* const environment = ::GetEnvironmentStringsW();
  if (environment == nullptr) {
    throw std::runtime_error("cannot inspect helper environment");
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
    throw std::runtime_error("cannot release helper environment");
  }
  const bool base_valid = !local_app_data.empty() && temp == tmp &&
                          temp == local_app_data + L"\\Temp";
  const bool isolated_environment =
      count == 3 && system_root.empty() && windir.empty();
  const bool development_environment =
      count == 5 && !system_root.empty() && windir == system_root;
  if (!base_valid) return EnvironmentBoundary::invalid;
  if (isolated_environment) {
    return EnvironmentBoundary::zero_network_app_container;
  }
  if (development_environment) {
    return EnvironmentBoundary::network_enabled_development;
  }
  return EnvironmentBoundary::invalid;
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

struct BlobReadResult final {
  OperationResult operation;
  usb_vault::security::SecureBuffer blob;
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

[[nodiscard]] OperationResult development_session(
    const DevelopmentSessionRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);
    DevelopmentSessionObservation observation{
        .attempts_before = session.user_pin_attempts_remaining(),
        .maximum_before = session.user_pin_attempts_maximum(),
    };
    const std::error_code observation_error =
        usb_vault::card::vikey::map_status(
            module.functions().get_user_password_attempt(
                session.index(), &observation.attempts_after,
                &observation.maximum_after));
    if (observation_error) {
      throw std::system_error(observation_error,
                              "recheck ViKey user PIN attempts");
    }
    if (observation.maximum_after != 0 &&
        observation.attempts_after > observation.maximum_after) {
      return {.status =
                  static_cast<std::int32_t>(CardError::device_failure)};
    }
    const std::error_code close_error = session.close();
    if (close_error) {
      return {.status = close_error.value()};
    }
    return {.detail = pack_development_session_observation(observation)};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value()};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] OperationResult development_blob_round_trip(
    const DevelopmentBlobRoundTripRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);

    OperationResult result{};
    try {
      usb_vault::card::vikey::verify_development_blob_round_trip(
          session, request.test_blob);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
    } catch (...) {
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status == static_cast<std::int32_t>(
                             CardError::device_failure)) {
      return result;
    }
    if (close_error) return {.status = close_error.value()};
    return result;
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value()};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] BlobReadResult development_blob_read(
    const DevelopmentSessionRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);

    usb_vault::security::SecureBuffer blob;
    OperationResult result{};
    try {
      blob = session.read_user_blob();
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
    } catch (...) {
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status != 0) return {.operation = result};
    if (close_error) {
      return {.operation = {.status = close_error.value()}};
    }
    if (blob.size() != usb_vault::card::vikey::kVikeyBlobBytes) {
      return {.operation = {.status = static_cast<std::int32_t>(
                                CardError::device_failure)}};
    }
    return {.blob = std::move(blob)};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.operation = {.status = error.code().value()}};
    }
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  } catch (...) {
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  }
}

[[nodiscard]] OperationResult development_blob_write(
    const DevelopmentBlobWriteRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);

    try {
      session.write_user_blob(request.blob);
    } catch (...) {
      static_cast<void>(session.close());
      // The vendor may have changed some or all bytes before reporting an
      // error. Never retry and never imply that the prior blob is intact.
      return {.status =
                  static_cast<std::int32_t>(CardError::device_failure)};
    }
    if (session.close()) {
      return {.status =
                  static_cast<std::int32_t>(CardError::device_failure)};
    }
    return {};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value()};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] BlobReadResult card_record_read(
    const DevelopmentSessionRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);

    usb_vault::security::SecureBuffer record;
    OperationResult result{};
    try {
      record = usb_vault::card::vikey::read_vikey_card_record(session);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
    } catch (...) {
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status != 0) return {.operation = result};
    if (close_error) {
      return {.operation = {.status = close_error.value()}};
    }
    if (record.size() != usb_vault::card::kCardRecordV1Size) {
      return {.operation = {.status = static_cast<std::int32_t>(
                                CardError::device_failure)}};
    }
    return {.blob = std::move(record)};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.operation = {.status = error.code().value()}};
    }
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  } catch (...) {
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  }
}

[[nodiscard]] OperationResult card_record_write(
    const CardRecordWriteRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);

    OperationResult result{};
    try {
      usb_vault::card::vikey::write_vikey_card_record(session,
                                                      request.record);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
    } catch (...) {
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status != 0) return result;
    if (close_error) return {.status = close_error.value()};
    return {};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value()};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] OperationResult operation_result(
    const Operation operation,
    const DevelopmentSessionRequest* const session_request = nullptr) {
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
    case Operation::development_session_probe:
      return session_request != nullptr
                 ? development_session(*session_request)
                 : OperationResult{
                       .status = static_cast<std::int32_t>(
                           CardError::invalid_operation)};
    case Operation::development_blob_round_trip_probe:
    case Operation::development_blob_read:
    case Operation::development_blob_write:
    case Operation::card_record_read:
    case Operation::card_record_write:
      return {.status =
                  static_cast<std::int32_t>(CardError::invalid_operation)};
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
    usb_vault::security::SecureBuffer response_blob;
    try {
      const EnvironmentBoundary environment = sanitized_environment();
      if (environment != EnvironmentBoundary::invalid) {
        OperationResult result{};
        if (request.operation == Operation::development_session_probe ||
            request.operation ==
                Operation::development_blob_round_trip_probe ||
            request.operation == Operation::development_blob_read ||
            request.operation == Operation::development_blob_write ||
            request.operation == Operation::card_record_read ||
            request.operation == Operation::card_record_write) {
          if (environment !=
              EnvironmentBoundary::network_enabled_development) {
            result.status =
                static_cast<std::int32_t>(CardError::invalid_operation);
          } else {
            if (request.operation == Operation::development_session_probe) {
              DevelopmentSessionRequest session_request =
                  read_development_session_request(request_handle, request);
              result = operation_result(request.operation, &session_request);
            } else {
              if (request.operation ==
                  Operation::development_blob_round_trip_probe) {
                DevelopmentBlobRoundTripRequest blob_request =
                    read_development_blob_round_trip_request(request_handle,
                                                             request);
                result = development_blob_round_trip(blob_request);
              } else if (request.operation ==
                         Operation::development_blob_read) {
                DevelopmentSessionRequest blob_request =
                    read_development_blob_read_request(request_handle,
                                                       request);
                BlobReadResult read_result =
                    development_blob_read(blob_request);
                result = read_result.operation;
                response_blob = std::move(read_result.blob);
              } else if (request.operation ==
                         Operation::development_blob_write) {
                DevelopmentBlobWriteRequest blob_request =
                    read_development_blob_write_request(request_handle,
                                                        request);
                result = development_blob_write(blob_request);
              } else if (request.operation == Operation::card_record_read) {
                DevelopmentSessionRequest record_request =
                    read_card_record_read_request(request_handle, request);
                BlobReadResult read_result =
                    card_record_read(record_request);
                result = read_result.operation;
                response_blob = std::move(read_result.blob);
              } else {
                CardRecordWriteRequest record_request =
                    read_card_record_write_request(request_handle, request);
                result = card_record_write(record_request);
              }
            }
            // The scoped request above wipes all helper-side credential
            // buffers before any response is emitted.
          }
        } else {
          result = operation_result(request.operation);
        }
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
        if (request.operation == Operation::development_blob_read) {
          response.payload_bytes =
              static_cast<std::uint32_t>(response_blob.size());
        } else if (request.operation == Operation::card_record_read) {
          response.payload_bytes =
              static_cast<std::uint32_t>(response_blob.size());
        }
      }
    } catch (...) {
      response.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
      response.security_properties = 0;
      response.detail = ERROR_GEN_FAILURE;
    }
    if (request.operation == Operation::development_blob_read &&
        response.status == 0) {
      write_development_blob_response(response_handle, response,
                                      response_blob);
    } else if (request.operation == Operation::card_record_read &&
               response.status == 0) {
      write_card_record_response(response_handle, response, response_blob);
    } else {
      write_response(response_handle, response, request.operation);
    }
    return 0;
  } catch (...) {
    return 2;
  }
}
