#include "card/vikey/vikey_module.h"
#include "vikey_test_artifact.h"

#include "usb_vault/card/card_error.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::make_error_code;
using usb_vault::card::vikey::DynamicModuleApi;
using usb_vault::card::vikey::VikeyModule;
using usb_vault::card::vikey::test::TemporaryVikeyArtifact;

static_assert(!std::is_copy_constructible_v<VikeyModule>);
static_assert(!std::is_move_constructible_v<VikeyModule>);

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct FakeModule final {
  const char* missing_symbol{};
  bool load_succeeds{true};
  std::uint32_t load_calls{};
  std::uint32_t resolve_calls{};
  std::uint32_t unload_calls{};
};

void* fake_load(void* const raw_context, const wchar_t*) noexcept {
  auto& context = *static_cast<FakeModule*>(raw_context);
  ++context.load_calls;
  if (!context.load_succeeds) {
    return nullptr;
  }
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000U));
}

void* fake_resolve(void* const raw_context, void*,
                   const char* const symbol_name) noexcept {
  auto& context = *static_cast<FakeModule*>(raw_context);
  ++context.resolve_calls;
  if (context.missing_symbol != nullptr &&
      std::strcmp(context.missing_symbol, symbol_name) == 0) {
    return nullptr;
  }
  return reinterpret_cast<void*>(
      static_cast<std::uintptr_t>(0x1000U + context.resolve_calls));
}

void fake_unload(void* const raw_context, void*) noexcept {
  auto& context = *static_cast<FakeModule*>(raw_context);
  ++context.unload_calls;
}

[[nodiscard]] DynamicModuleApi fake_api(FakeModule& context) noexcept {
  return {
      .context = &context,
      .load = &fake_load,
      .resolve = &fake_resolve,
      .unload = &fake_unload,
  };
}

template <typename Operation>
void require_card_error(Operation&& operation, const CardError expected,
                        const char* const message) {
  try {
    std::forward<Operation>(operation)();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

void test_incomplete_module_api_is_rejected_before_loading() {
  TemporaryVikeyArtifact artifact;
  FakeModule context;
  DynamicModuleApi incomplete = fake_api(context);
  incomplete.resolve = nullptr;
  require_card_error(
      [&] {
        static_cast<void>(VikeyModule::load_from(artifact.verified(),
                                                 incomplete));
      },
      CardError::invalid_configuration,
      "incomplete dynamic-module API was not rejected");
  require(context.load_calls == 0,
          "incomplete dynamic-module API reached the loader");
}

void test_load_failure_uses_generic_error_domain() {
  TemporaryVikeyArtifact artifact;
  FakeModule context{.load_succeeds = false};
  require_card_error(
      [&] {
        static_cast<void>(VikeyModule::load_from(artifact.verified(),
                                                 fake_api(context)));
      },
      CardError::backend_unavailable,
      "module load failure escaped the generic card error domain");
  require(context.load_calls == 1, "module load was not attempted once");
  require(context.resolve_calls == 0,
          "exports were resolved after module load failure");
  require(context.unload_calls == 0,
          "failed module load attempted an unload");
}

void test_missing_export_fails_closed_and_unloads() {
  TemporaryVikeyArtifact artifact;
  FakeModule context{.missing_symbol = "VikeyWriteData"};
  require_card_error(
      [&] {
        static_cast<void>(VikeyModule::load_from(artifact.verified(),
                                                 fake_api(context)));
      },
      CardError::backend_unavailable,
      "missing ViKey export did not fail closed");
  require(context.load_calls == 1, "module was not loaded once");
  require(context.unload_calls == 1,
          "partially resolved module was not unloaded");
}

void test_complete_export_table_owns_one_module_lifetime() {
  TemporaryVikeyArtifact artifact;
  FakeModule context;
  {
    VikeyModule module =
        VikeyModule::load_from(artifact.verified(), fake_api(context));
    const auto& functions = module.functions();
    require(functions.set_user_sn != nullptr && functions.find_ex != nullptr &&
                functions.set_api_key != nullptr &&
                functions.uninitialization != nullptr &&
                functions.get_hid != nullptr &&
                functions.get_level != nullptr &&
                functions.get_user_data_size != nullptr &&
                functions.get_user_password_attempt != nullptr &&
                functions.user_login != nullptr && functions.logoff != nullptr &&
                functions.read_data != nullptr &&
                functions.write_data != nullptr &&
                functions.get_module != nullptr &&
                functions.decrease_module != nullptr &&
                functions.check_module != nullptr,
            "complete ViKey export table contained a null function");
    require(context.resolve_calls == 15,
            "ViKey loader resolved an unexpected export count");

    require(context.unload_calls == 0,
            "ViKey module unloaded before leaving scope");
    require(!artifact.can_open_for_write(),
            "ViKey write lock did not follow the module lifetime");
    require(!artifact.can_open_for_delete(),
            "ViKey delete lock did not follow the module lifetime");
    require(!artifact.can_round_trip_rename_directory(),
            "ViKey directory lock did not follow the module lifetime");
    require(!artifact.can_round_trip_rename_ancestor(),
            "ViKey ancestor lock did not follow the module lifetime");
  }
  require(context.unload_calls == 1,
          "ViKey module lifetime did not unload exactly once");
  require(artifact.can_open_for_write(),
          "ViKey write lock outlived the module lifetime");
  require(artifact.can_open_for_delete(),
          "ViKey delete lock outlived the module lifetime");
  require(artifact.can_round_trip_rename_directory(),
          "ViKey directory lock outlived the module lifetime");
  require(artifact.can_round_trip_rename_ancestor(),
          "ViKey ancestor lock outlived the module lifetime");
}

void test_vendor_statuses_map_to_generic_card_errors() {
  namespace sdk = usb_vault::card::vikey::sdk;
  using usb_vault::card::vikey::map_status;

  require(!map_status(sdk::kSuccess), "success mapped to an error");
  require(map_status(sdk::kNoDevice) ==
              make_error_code(CardError::device_not_found),
          "no-device status mapping differed");
  require(map_status(sdk::kInvalidPassword) ==
              make_error_code(CardError::authentication_failed),
          "invalid-password status mapping differed");
  require(map_status(sdk::kNoChance) ==
              make_error_code(CardError::authentication_locked),
          "no-chance status mapping differed");
  require(map_status(sdk::kNoAuthority) ==
              make_error_code(CardError::permission_denied),
          "authority status mapping differed");
  require(map_status(sdk::kNoSupport) ==
              make_error_code(CardError::unsupported),
          "unsupported status mapping differed");
  require(map_status(sdk::kCommunication) ==
              make_error_code(CardError::communication_failed),
          "communication status mapping differed");
  require(map_status(sdk::kInvalidAddressOrSize) ==
              make_error_code(CardError::invalid_operation),
          "invalid-range status mapping differed");
  require(map_status(sdk::kApiKey) ==
              make_error_code(CardError::invalid_configuration),
          "API-key status mapping differed");
  require(map_status(0x81234567U) ==
              make_error_code(CardError::device_failure),
          "unknown vendor status did not fail closed");
}

}  // namespace

int main() {
  try {
    test_incomplete_module_api_is_rejected_before_loading();
    test_load_failure_uses_generic_error_domain();
    test_missing_export_fails_closed_and_unloads();
    test_complete_export_table_owns_one_module_lifetime();
    test_vendor_statuses_map_to_generic_card_errors();
  } catch (const std::exception& error) {
    std::cerr << "vikey_module_tests: FAIL: " << error.what() << '\n';
    return 1;
  }

  std::cout << "vikey_module_tests: PASS\n";
  return 0;
}
