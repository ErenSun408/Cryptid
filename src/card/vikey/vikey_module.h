#pragma once

#include "card/vikey/vikey_sdk_abi.h"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace usb_vault::card::vikey {

// Injectable OS module operations keep export resolution testable without
// loading the unsigned vendor DLL. This is internal to the ViKey adapter.
struct DynamicModuleApi final {
  void* context{};
  void* (*load)(void* context, const wchar_t* absolute_path) noexcept{};
  void* (*resolve)(void* context, void* module,
                   const char* symbol_name) noexcept{};
  void (*unload)(void* context, void* module) noexcept{};
};

[[nodiscard]] DynamicModuleApi windows_dynamic_module_api() noexcept;
[[nodiscard]] std::error_code map_status(sdk::Status status) noexcept;

class VikeyModule final {
 public:
  ~VikeyModule() noexcept;

  VikeyModule(const VikeyModule&) = delete;
  VikeyModule& operator=(const VikeyModule&) = delete;
  VikeyModule(VikeyModule&&) = delete;
  VikeyModule& operator=(VikeyModule&&) = delete;

  [[nodiscard]] static VikeyModule load_from(
      const std::filesystem::path& absolute_path,
      DynamicModuleApi module_api = windows_dynamic_module_api());

  [[nodiscard]] const sdk::Functions& functions() const noexcept;

 private:
  VikeyModule(void* module, DynamicModuleApi module_api,
              sdk::Functions functions) noexcept;

  void* module_{};
  DynamicModuleApi module_api_{};
  sdk::Functions functions_{};
};

}  // namespace usb_vault::card::vikey
