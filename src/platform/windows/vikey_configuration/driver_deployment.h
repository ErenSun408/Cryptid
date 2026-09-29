#pragma once
#include "setup_policy.h"
#include <filesystem>

namespace configuration::driver {
// All pre-existing installations are shared. Never infer exclusive ownership
// from the service name, ABI, or an unprivileged application's local record.
enum class Presence { absent, trusted_running, trusted_stopped, existing_unavailable, incompatible, unknown };
constexpr setup::Result classify(Presence presence, bool orphan_file) noexcept {
  if (presence == Presence::absent)
    return orphan_file ? setup::Result::unavailable : setup::Result::missing;
  if (presence == Presence::trusted_running) return setup::Result::ready;
  if (presence == Presence::trusted_stopped) return setup::Result::repairable;
  if (presence == Presence::incompatible) return setup::Result::incompatible;
  return setup::Result::unavailable;
}
std::filesystem::path system_driver_path();
setup::Result probe() noexcept;
setup::Result install_elevated(const std::filesystem::path& directory) noexcept;
setup::Result request_install(const std::filesystem::path& executable) noexcept;
}
