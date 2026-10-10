#include "card/diagnostic_error.h"
#include "card/vikey/helper_environment.h"
using usb_vault::card::diagnostic_native;
using usb_vault::card::diagnostic_stage;
using usb_vault::card::diagnostic_kind;
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

[[nodiscard]] EnvironmentCheck sanitized_environment() noexcept {
  return inspect_helper_environment(
      [] { return ::GetEnvironmentStringsW(); },
      [](wchar_t* block) { return ::FreeEnvironmentStringsW(block) != FALSE; },
      [] { return static_cast<std::uint32_t>(::GetLastError()); });
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
  std::uint32_t result{};
  std::uint32_t logical_state{};
  std::uint32_t diagnostic{};
  std::uint32_t native_kind{};
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] OperationResult card_identity_probe(
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
    const std::uint32_t hardware_id = session.hardware_id();
    const std::error_code close_error = session.close();
    if (close_error) return {.status = close_error.value()};
    return {.detail = hardware_id};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] std::uint32_t wire_card_media_state(
    const usb_vault::card::vikey::VikeyCardMediaState state) {
  switch (state) {
    case usb_vault::card::vikey::VikeyCardMediaState::blank:
      return static_cast<std::uint32_t>(CardMediaState::blank);
    case usb_vault::card::vikey::VikeyCardMediaState::
        legacy_p1_or_foreign_prefix:
      return static_cast<std::uint32_t>(
          CardMediaState::legacy_p1_or_foreign_prefix);
    case usb_vault::card::vikey::VikeyCardMediaState::foreign_data:
      return static_cast<std::uint32_t>(CardMediaState::foreign_data);
    case usb_vault::card::vikey::VikeyCardMediaState::
        provisioning_interrupted:
      return static_cast<std::uint32_t>(
          CardMediaState::provisioning_interrupted);
    case usb_vault::card::vikey::VikeyCardMediaState::conflicting_evidence:
      return static_cast<std::uint32_t>(
          CardMediaState::conflicting_evidence);
    case usb_vault::card::vikey::VikeyCardMediaState::product_evidence:
      return static_cast<std::uint32_t>(CardMediaState::product_evidence);
  }
  throw std::system_error(
      usb_vault::card::make_error_code(CardError::device_failure),
      "ViKey helper observed an invalid card media state");
}

[[nodiscard]] OperationResult card_media_probe(
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
    const std::uint32_t hardware_id = session.hardware_id();

    OperationResult result{};
    try {
      const auto media_state =
          usb_vault::card::vikey::inspect_vikey_card_media(session);
      result.result = wire_card_media_state(media_state);
      if (media_state == usb_vault::card::vikey::
                             VikeyCardMediaState::product_evidence) {
        switch (usb_vault::card::vikey::
                    inspect_vikey_card_record_state_read_only(session)) {
          case usb_vault::card::CardRecordState::provisioned_uninitialized:
            result.logical_state = static_cast<std::uint32_t>(
                CardLogicalState::provisioned_uninitialized);
            break;
          case usb_vault::card::CardRecordState::initialized_v1:
            result.logical_state = static_cast<std::uint32_t>(
                CardLogicalState::initialized_v1);
            break;
          case usb_vault::card::CardRecordState::unsupported_version:
            result.logical_state = static_cast<std::uint32_t>(
                CardLogicalState::unsupported_version);
            break;
          case usb_vault::card::CardRecordState::malformed:
            result.logical_state = static_cast<std::uint32_t>(
                CardLogicalState::malformed);
            break;
          case usb_vault::card::CardRecordState::legacy_p1_blob:
          default:
            throw std::system_error(
                usb_vault::card::make_error_code(CardError::device_failure),
                "ViKey journal returned an impossible logical record state");
        }
      }
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
    } catch (...) {
      result.diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::unexpected);
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status != 0) {
      result.result = 0;
      result.logical_state = 0;
      return result;
    }
    if (close_error) return {.status = close_error.value(), .diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::cleanup)};
    result.detail = hardware_id;
    return result;
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable), .diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::unexpected)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable), .diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::unexpected)};
  }
}

void require_expected_hardware_identity(
    const usb_vault::card::vikey::VikeySession& session,
    const std::uint32_t expected_hardware_id) {
  if (expected_hardware_id == 0 ||
      session.hardware_id() != expected_hardware_id) {
    throw std::system_error(
        usb_vault::card::make_error_code(CardError::device_failure),
        "ViKey helper opened an unexpected hardware identity");
  }
}

[[nodiscard]] usb_vault::card::vikey::VikeyCardMediaState
bootstrap_media_state(const CardMediaState state) {
  switch (state) {
    case CardMediaState::blank:
      return usb_vault::card::vikey::VikeyCardMediaState::blank;
    case CardMediaState::provisioning_interrupted:
      return usb_vault::card::vikey::
          VikeyCardMediaState::provisioning_interrupted;
    default:
      throw std::system_error(
          usb_vault::card::make_error_code(
              CardError::invalid_configuration),
          "ViKey helper received an ineligible bootstrap media state");
  }
}

[[nodiscard]] OperationResult card_media_bootstrap(
    const CardMediaBootstrapRequest& request) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);
    require_expected_hardware_identity(session,
                                       request.expected_hardware_id);

    OperationResult result{};
    try {
      usb_vault::card::vikey::bootstrap_vikey_card_media(
          session, bootstrap_media_state(request.expected_media_state));
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] OperationResult development_media_reset(
    const CardMediaBootstrapRequest& request) {
  try {
    if (request.expected_media_state !=
        CardMediaState::legacy_p1_or_foreign_prefix) {
      return {.status = static_cast<std::int32_t>(
                  CardError::invalid_configuration)};
    }
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);
    require_expected_hardware_identity(session,
                                       request.expected_hardware_id);

    OperationResult result{};
    try {
      usb_vault::card::vikey::reset_vikey_development_legacy_media(
          session,
          usb_vault::card::vikey::
              VikeyCardMediaState::legacy_p1_or_foreign_prefix);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
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
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
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
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
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
      return {.operation = {.status = error.code().value(), .detail = diagnostic_native(error), .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)}};
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
    }
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  } catch (...) {
    return {.status =
                static_cast<std::int32_t>(CardError::backend_unavailable)};
  }
}

[[nodiscard]] BlobReadResult card_record_read(
    const CardRecordSessionRequest& request, bool snapshot) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);
    require_expected_hardware_identity(session,
                                       request.expected_hardware_id);

    usb_vault::security::SecureBuffer record;
    OperationResult result{};
    try {
      record = snapshot ? usb_vault::card::vikey::snapshot_vikey_card_record(session)
                        : usb_vault::card::vikey::read_vikey_card_record(session);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
    } catch (...) {
      result.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
    }

    const std::error_code close_error = session.close();
    if (result.status != 0) return {.operation = result};
    if (close_error) {
      return {.operation = {.status = close_error.value()}};
    }
    if (!valid_record_payload(Operation::card_record_read, record.size())) {
      return {.operation = {.status = static_cast<std::int32_t>(
                                CardError::device_failure)}};
    }
    return {.blob = std::move(record)};
  } catch (const std::system_error& error) {
    if (error.code().category() == card_error_category()) {
      return {.operation = {.status = error.code().value(), .detail = diagnostic_native(error), .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)}};
    }
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  } catch (...) {
    return {.operation = {.status = static_cast<std::int32_t>(
                              CardError::backend_unavailable)}};
  }
}

[[nodiscard]] OperationResult card_record_write(
    const CardRecordWriteRequest& request, bool reset) {
  try {
    const std::filesystem::path staged = staged_vendor_dll();
    auto verified = usb_vault::card::vikey::verify_vikey_dll(
        staged.parent_path(), request.vendor_digest);
    auto module =
        usb_vault::card::vikey::VikeyModule::load_from(std::move(verified));
    auto session = usb_vault::card::vikey::VikeySession::open(
        module.functions(), request.user_sn, request.api_key,
        request.user_pin);
    require_expected_hardware_identity(session,
                                       request.expected_hardware_id);

    OperationResult result{};
    try {
      if (reset) usb_vault::card::vikey::reset_vikey_card_record(session, request.record);
      else usb_vault::card::vikey::write_vikey_card_record(session, request.record);
    } catch (const std::system_error& error) {
      result.status = error.code().category() == card_error_category()
                          ? error.code().value()
                          : static_cast<std::int32_t>(
                                CardError::backend_unavailable);
      result.detail = diagnostic_native(error);
      result.diagnostic = diagnostic_stage(error);
      result.native_kind = diagnostic_kind(error);
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
      return {.status = error.code().value(), .detail = diagnostic_native(error),
              .diagnostic = diagnostic_stage(error), .native_kind = diagnostic_kind(error)};
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
    case Operation::card_identity_probe:
      return session_request != nullptr
                 ? card_identity_probe(*session_request)
                 : OperationResult{
                       .status = static_cast<std::int32_t>(
                           CardError::invalid_operation)};
    case Operation::card_media_probe:
      return session_request != nullptr
                 ? card_media_probe(*session_request)
                 : OperationResult{
                       .status = static_cast<std::int32_t>(
                           CardError::invalid_operation)};
    case Operation::development_blob_round_trip_probe:
    case Operation::development_blob_read:
    case Operation::development_blob_write:
    case Operation::card_record_read:
    case Operation::card_record_snapshot:
    case Operation::card_record_write:
    case Operation::card_record_reset:
    case Operation::card_media_bootstrap:
    case Operation::development_media_reset:
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
      const auto environment_check = sanitized_environment();
      const auto environment = environment_check.boundary;
      if (environment != EnvironmentBoundary::invalid) {
        OperationResult result{};
        if (request.operation == Operation::development_session_probe ||
            request.operation ==
                Operation::development_blob_round_trip_probe ||
            request.operation == Operation::development_blob_read ||
            request.operation == Operation::development_blob_write ||
            is_record_read(request.operation) ||
            is_record_write(request.operation) ||
            request.operation == Operation::card_identity_probe ||
            request.operation == Operation::card_media_probe ||
            request.operation == Operation::card_media_bootstrap ||
            request.operation == Operation::development_media_reset) {
          if (environment !=
              EnvironmentBoundary::network_enabled_development) {
            result.status =
                static_cast<std::int32_t>(CardError::invalid_operation);
          } else {
            if (request.operation == Operation::development_session_probe) {
              DevelopmentSessionRequest session_request =
                  read_development_session_request(request_handle, request);
              result = operation_result(request.operation, &session_request);
            } else if (request.operation ==
                       Operation::card_identity_probe) {
              DevelopmentSessionRequest identity_request =
                  read_card_identity_probe_request(request_handle, request);
              result = operation_result(request.operation,
                                        &identity_request);
            } else if (request.operation ==
                       Operation::card_media_probe) {
              DevelopmentSessionRequest media_request =
                  read_card_media_probe_request(request_handle, request);
              result = operation_result(request.operation,
                                        &media_request);
            } else if (request.operation ==
                       Operation::card_media_bootstrap) {
              CardMediaBootstrapRequest bootstrap_request =
                  read_card_media_bootstrap_request(request_handle,
                                                    request);
              result = card_media_bootstrap(bootstrap_request);
            } else if (request.operation ==
                       Operation::development_media_reset) {
              CardMediaBootstrapRequest reset_request =
                  read_development_media_reset_request(request_handle,
                                                       request);
              result = development_media_reset(reset_request);
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
              } else if (is_record_read(request.operation)) {
                CardRecordSessionRequest record_request =
                    read_card_record_read_request(request_handle, request);
                BlobReadResult read_result =
                    card_record_read(record_request, request.operation == Operation::card_record_snapshot);
                result = read_result.operation;
                response_blob = std::move(read_result.blob);
              } else {
                CardRecordWriteRequest record_request =
                    read_card_record_write_request(request_handle, request);
                result = card_record_write(record_request, request.operation == Operation::card_record_reset);
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
        response.result = result.result;
        response.logical_state = result.logical_state;
        response.diagnostic = result.diagnostic;
        response.native_kind = result.native_kind;
      } else {
        response.status =
            static_cast<std::int32_t>(CardError::backend_unavailable);
        response.detail = environment_check.native;
        response.security_step = environment_check.step;
        response.diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::process_security);
        response.native_kind = environment_check.step >= 107
            ? static_cast<std::uint32_t>(usb_vault::card::NativeCodeKind::win32) : 0;
      }
      if (response.status == 0) {
        response.security_properties =
            property_bit(SecurityProperty::sanitized_environment);
        if (request.operation == Operation::development_blob_read) {
          response.payload_bytes =
              static_cast<std::uint32_t>(response_blob.size());
        } else if (is_record_read(request.operation)) {
          response.payload_bytes =
              static_cast<std::uint32_t>(response_blob.size());
        }
      }
    } catch (...) {
      response.status =
          static_cast<std::int32_t>(CardError::backend_unavailable);
      response.security_properties = 0;
      response.payload_bytes = 0;
      response.security_step = 0;
      response.detail = ERROR_GEN_FAILURE;
      response.diagnostic = static_cast<std::uint32_t>(usb_vault::card::DiagnosticStage::unexpected);
      response.native_kind = static_cast<std::uint32_t>(usb_vault::card::NativeCodeKind::win32);
      response.result = 0;
      response.logical_state = 0;
    }
    if (request.operation == Operation::development_blob_read &&
        response.status == 0) {
      write_development_blob_response(response_handle, response,
                                      response_blob);
    } else if (is_record_read(request.operation) &&
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
