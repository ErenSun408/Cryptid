#include "card/vikey/vikey_module.h"

#include "usb_vault/card/card_error.h"

#include <Windows.h>

#include <system_error>
#include <utility>

namespace usb_vault::card::vikey {
namespace {

void* windows_load(void*, const wchar_t* const absolute_path) noexcept {
  return ::LoadLibraryExW(
      absolute_path, nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

void* windows_resolve(void*, void* const module,
                      const char* const symbol_name) noexcept {
  return reinterpret_cast<void*>(
      ::GetProcAddress(static_cast<HMODULE>(module), symbol_name));
}

void windows_unload(void*, void* const module) noexcept {
  static_cast<void>(::FreeLibrary(static_cast<HMODULE>(module)));
}

[[noreturn]] void fail(const CardError error, const char* const context) {
  throw std::system_error(make_error_code(error), context);
}

void validate(const DynamicModuleApi& module_api) {
  if (module_api.load == nullptr || module_api.resolve == nullptr ||
      module_api.unload == nullptr) {
    fail(CardError::invalid_configuration,
         "ViKey dynamic module operations are incomplete");
  }
}

template <typename Function>
[[nodiscard]] Function required_symbol(const DynamicModuleApi& module_api,
                                       void* const module,
                                       const char* const symbol_name) {
  void* const symbol =
      module_api.resolve(module_api.context, module, symbol_name);
  if (symbol == nullptr) {
    fail(CardError::backend_unavailable,
         "ViKey DLL is missing a required export");
  }
  return reinterpret_cast<Function>(symbol);
}

[[nodiscard]] sdk::Functions resolve_functions(
    const DynamicModuleApi& module_api, void* const module) {
  return {
      .set_user_sn = required_symbol<sdk::SetUserSn>(
          module_api, module, "VikeySetUserSN"),
      .find_ex = required_symbol<sdk::FindEx>(module_api, module,
                                             "VikeyFindEx"),
      .set_api_key = required_symbol<sdk::SetApiKey>(
          module_api, module, "VikeySetApiKey"),
      .uninitialization = required_symbol<sdk::Uninitialization>(
          module_api, module, "VikeyUninitialization"),
      .get_hid = required_symbol<sdk::GetHid>(module_api, module,
                                             "VikeyGetHID"),
      .get_level = required_symbol<sdk::GetLevel>(module_api, module,
                                                 "VikeyGetLevel"),
      .get_user_data_size = required_symbol<sdk::GetUserDataSize>(
          module_api, module, "VikeyGetUserDataSize"),
      .get_user_password_attempt =
          required_symbol<sdk::GetUserPasswordAttempt>(
              module_api, module, "VikeyGetUserPassWordAttempt"),
      .user_login = required_symbol<sdk::UserLogin>(
          module_api, module, "VikeyUserLogin"),
      .logoff = required_symbol<sdk::Logoff>(module_api, module,
                                            "VikeyLogoff"),
      .read_data = required_symbol<sdk::ReadData>(module_api, module,
                                                 "VikeyReadData"),
      .write_data = required_symbol<sdk::WriteData>(module_api, module,
                                                   "VikeyWriteData"),
      .get_module = required_symbol<sdk::GetModule>(module_api, module,
                                                   "ViKeyGetModule"),
      .decrease_module = required_symbol<sdk::DecreaseModule>(
          module_api, module, "ViKeyDecraseModule"),
      .check_module = required_symbol<sdk::CheckModule>(
          module_api, module, "ViKeyCheckModule"),
  };
}

class PendingModule final {
 public:
  PendingModule(void* const module, const DynamicModuleApi module_api) noexcept
      : module_(module), module_api_(module_api) {}

  ~PendingModule() noexcept {
    if (module_ != nullptr) {
      module_api_.unload(module_api_.context, module_);
    }
  }

  PendingModule(const PendingModule&) = delete;
  PendingModule& operator=(const PendingModule&) = delete;

  [[nodiscard]] void* get() const noexcept { return module_; }

  [[nodiscard]] void* release() noexcept {
    return std::exchange(module_, nullptr);
  }

 private:
  void* module_{};
  DynamicModuleApi module_api_{};
};

}  // namespace

DynamicModuleApi windows_dynamic_module_api() noexcept {
  return {
      .context = nullptr,
      .load = &windows_load,
      .resolve = &windows_resolve,
      .unload = &windows_unload,
  };
}

std::error_code map_status(const sdk::Status status) noexcept {
  switch (status) {
    case sdk::kSuccess:
      return {};
    case sdk::kNoDevice:
      return make_error_code(CardError::device_not_found);
    case sdk::kInvalidPassword:
      return make_error_code(CardError::authentication_failed);
    case sdk::kNoChance:
      return make_error_code(CardError::authentication_locked);
    case sdk::kNoAuthority:
      return make_error_code(CardError::permission_denied);
    case sdk::kNoSupport:
      return make_error_code(CardError::unsupported);
    case sdk::kConnection:
    case sdk::kGetUsbData:
    case sdk::kSetUsbData:
    case sdk::kCommunication:
      return make_error_code(CardError::communication_failed);
    case sdk::kNeedFind:
    case sdk::kInvalidIndex:
    case sdk::kInvalidValue:
    case sdk::kInvalidAddressOrSize:
      return make_error_code(CardError::invalid_operation);
    case sdk::kApiKey:
    case sdk::kUserSn:
    case sdk::kNotActive:
    case sdk::kUnsetApiKey:
    case sdk::kApiKeyCalculate1:
    case sdk::kApiKeyCalculate2:
    case sdk::kCheckApiKey:
      return make_error_code(CardError::invalid_configuration);
    default:
      return make_error_code(CardError::device_failure);
  }
}

VikeyModule::VikeyModule(VerifiedVikeyDll&& verified_dll,
                         void* const module,
                         const DynamicModuleApi module_api,
                         sdk::Functions functions) noexcept
    : verified_dll_(std::move(verified_dll)),
      module_(module),
      module_api_(module_api),
      functions_(std::move(functions)) {}

VikeyModule::~VikeyModule() noexcept {
  if (module_ != nullptr) {
    module_api_.unload(module_api_.context, module_);
  }
}

VikeyModule VikeyModule::load_from(VerifiedVikeyDll&& verified_dll,
                                   const DynamicModuleApi module_api) {
  validate(module_api);
  PendingModule pending(
      module_api.load(module_api.context, verified_dll.path().c_str()),
      module_api);
  if (pending.get() == nullptr) {
    fail(CardError::backend_unavailable, "load ViKey DLL");
  }

  sdk::Functions functions = resolve_functions(module_api, pending.get());
  return VikeyModule(std::move(verified_dll), pending.release(), module_api,
                     functions);
}

const sdk::Functions& VikeyModule::functions() const noexcept {
  return functions_;
}

}  // namespace usb_vault::card::vikey
