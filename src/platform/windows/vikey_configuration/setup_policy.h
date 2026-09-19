#pragma once

namespace configuration::setup {
// Stable process exit codes shared with the bootstrap installer.
enum class Result : int {
  ready = 0, missing = 10, incompatible = 11, unavailable = 12,
  restart = 20, cancelled = 21, failed = 22, bad_arguments = 64
};

template <class Probe, class Install>
Result ensure_dependency(Probe probe, Install install) {
  const auto before = probe();
  if (before != Result::missing) return before;
  const auto installed = install();
  if (installed != Result::ready) return installed;
  // An installer returning success is not proof that the dependency works.
  const auto after = probe();
  return after == Result::missing ? Result::failed : after;
}
}
