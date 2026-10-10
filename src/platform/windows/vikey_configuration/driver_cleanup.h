#pragma once
#include "setup_policy.h"
#include <filesystem>

namespace configuration::driver_cleanup {
enum class State { absent, stopped, running, orphan, blocked };
// Sequencing is shared by production operations and isolated fault injection.
template<class Operations> setup::Result remove(Operations& ops) {
  using setup::Result;
  const auto state = ops.inspect();
  if (state == State::absent) return Result::ready;
  if (state == State::blocked || !ops.verify()) return Result::unavailable;
  if (state == State::running && (!ops.idle() || !ops.stop())) return Result::unavailable;
  if (!ops.stopped() || !ops.prepare_delete()) return Result::unavailable;
  if (state != State::orphan && !ops.delete_service()) return Result::failed;
  if (!ops.delete_file()) return Result::failed;
  return ops.absent() ? Result::ready : Result::failed;
}
setup::Result uninstall_elevated() noexcept;
setup::Result request_uninstall(const std::filesystem::path& executable) noexcept;
}
