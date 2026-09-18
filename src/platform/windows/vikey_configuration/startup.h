#pragma once

#include <functional>

namespace configuration {
using EnvironmentCheck = std::function<bool()>;
using SetupWindow = std::function<bool()>;

// Start gate for the product session. The environment check never touches the
// security card, so starting the product cannot consume a card attempt on its
// own. Setup opens at most once per start and only after a check that did not
// pass; a check that cannot run counts as not ready, and setup that cannot run
// keeps the session closed instead of starting an unchecked environment.
inline bool ready_to_start(const EnvironmentCheck& check,
                           const SetupWindow& setup) noexcept {
  bool ready = false;
  try {
    ready = check();
  } catch (...) {
    ready = false;
  }
  if (ready) return true;
  try {
    return setup();
  } catch (...) {
    return false;
  }
}
}  // namespace configuration
